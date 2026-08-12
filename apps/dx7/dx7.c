/*
 * DX7 — polyphonic structural 6-op FM synth for ArgonOS.
 *
 * Sound via audio_out device path (default /dev/pcmnull). Use pcmvirt after
 * `drv install` of PCMVIRT.SYS for host playback (tools/pcmplay.py).
 * Optional live notes: MIDIVIRT.SYS → /dev/midivirt + tools/midikbd.py :5559.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>

#include <argon/argon.h>
#include <argon/keys.h>

#include "audio_out.h"
#include "ag_dx7.h"
#include "ag_mid.h"
#include "ag_fx.h"

AG_APP_SIZED("DX7", "0.5", "argon", 0, 10 * 1024, 192 * 1024);

#define RATE 22050u
#define CHUNK 441 /* ~20 ms @ 22050 */
#define CHUNK_US ((uint32_t)((CHUNK * 1000000ull) / RATE))
/* Only start a UI redraw if this much slack remains before the audio due. */
#define UI_SLACK_US 4000u
#define UI_PERIOD_MS 250u

enum {
    FX_OFF = 0,
    FX_LOCAL = 1,
    FX_CORE1 = 2
};

static ag_dx7_t s_dx;
static int16_t s_pcm[CHUNK * 2];
static int s_preset;
static int s_notes_down;
static uint8_t s_key_held[32]; /* parallel to k_keys[] */
static uint8_t s_have_keyup;   /* set when terminal sends real KEY_UP */
static uint8_t s_last_note;    /* last MIDI note triggered (debug) */
static uint8_t s_note_held[128]; /* any source: kbd / midivirt / SMF */
static int s_held_n;
static int s_dirty = 1;

/* Live MIDI-in from /dev/midivirt (host midikbd.py). */
static ag_handle_t s_midi_fd = -1;
static int s_midi_want = 1; /* default: try open */
static uint32_t s_midi_ev;  /* events consumed (UI) */

/* Master-bus FX (lazy-init buffers on first fx0/fx1). */
static ag_fx_t s_fx;
static int s_fx_ready;
static int s_fx_mode = FX_OFF; /* default: dry */
static int s_fx_want = FX_OFF; /* from args; applied after open */
static uint32_t s_fx_late;     /* core1 wet timeout / underrun */
static int16_t s_fx_dry[2][CHUNK * 2];
static int16_t s_fx_wet[2][CHUNK * 2];
static ag_queue_t s_q_dry;
static ag_queue_t s_q_wet;
static ag_thread_t s_fx_thr;
static volatile int s_fx_run;
static int s_fx_slot;
static int s_fx_primed; /* core1: have a wet chunk to play */

/* SysEx bank (packed 128 × N). */
enum { SYX_MAX = 16, SYX_PATH_LEN = 96 };
static uint8_t s_bank[AG_DX7_BANK_VOICES][AG_DX7_PACKED_VOICE];
static int s_bank_n;
static int s_bank_voice;
static int s_use_bank; /* ,/. cycle bank when set */
static char s_bank_path[SYX_PATH_LEN];
static char s_syx_list[SYX_MAX][SYX_PATH_LEN];
static int s_syx_n;
static int s_syx_i;

/* MIDI file player (loop). */
static ag_mid_player_t s_mid;
static char s_mid_path[SYX_PATH_LEN];
static char s_mid_err[80];
static int s_mid_loaded;
static uint32_t s_mid_notes; /* note-ons fired (UI) */

static char s_audio_path[AG_PATH_MAX];
static ag_handle_t s_audio_fd = -1;
static int s_send_err_reported;

/* Perf / stream health (shown in UI). */
static uint32_t s_render_us;
static uint32_t s_send_us;
static uint32_t s_loop_us;
static uint32_t s_load_pct; /* render / chunk budget */
static uint32_t s_late;     /* paced deadline missed */
static uint32_t s_drop;     /* bytes we failed to push in time */
static uint32_t s_resync;
static ag_audio_stats_t s_pcm_stats; /* from AG_IOC_AUDIO_GETSTATS when supported */
static int s_pcm_stats_ok;
static ag_time_t s_next_due;
static uint32_t s_ui_ms;

typedef struct {
    int key;
    int note;
} key_map_t;

/*
 * Fixed concert map (ignores patch transpose):
 *   Z..M  = C4..B4  (до..си одной октавы, с чёрными S D G H J)
 *   Q..I  = C5..C6
 */
static const key_map_t k_keys[] = {
    {AG_KEY_Z, 60}, {AG_KEY_S, 61}, {AG_KEY_X, 62}, {AG_KEY_D, 63},
    {AG_KEY_C, 64}, {AG_KEY_V, 65}, {AG_KEY_G, 66}, {AG_KEY_B, 67},
    {AG_KEY_H, 68}, {AG_KEY_N, 69}, {AG_KEY_J, 70}, {AG_KEY_M, 71},
    {AG_KEY_Q, 72}, {AG_KEY_2, 73}, {AG_KEY_W, 74}, {AG_KEY_3, 75},
    {AG_KEY_E, 76}, {AG_KEY_R, 77}, {AG_KEY_5, 78}, {AG_KEY_T, 79},
    {AG_KEY_6, 80}, {AG_KEY_Y, 81}, {AG_KEY_7, 82}, {AG_KEY_U, 83},
    {AG_KEY_I, 84},
};

enum { K_KEYS_N = (int)(sizeof(k_keys) / sizeof(k_keys[0])) };

static int str_ieq(const char *s, const char *lit)
{
    if (s == NULL || lit == NULL) {
        return 0;
    }
    for (; *lit && *s; lit++, s++) {
        char c = *s;
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if (*lit != c) {
            return 0;
        }
    }
    return *lit == '\0' && *s == '\0';
}

static void piano_all_off(void);
static void select_bank_voice(int idx);

static int ends_with_ci(const char *s, const char *ext4)
{
    int n, i;
    if (s == NULL || ext4 == NULL) {
        return 0;
    }
    for (n = 0; s[n]; n++) {
    }
    if (n < 4) {
        return 0;
    }
    for (i = 0; i < 4; i++) {
        char a = s[n - 4 + i];
        char b = ext4[i];
        if (a >= 'A' && a <= 'Z') {
            a = (char)(a - 'A' + 'a');
        }
        if (a != b) {
            return 0;
        }
    }
    return 1;
}

static int ends_with_syx(const char *s)
{
    return ends_with_ci(s, ".syx");
}

static int ends_with_mid(const char *s)
{
    return ends_with_ci(s, ".mid");
}

static void path_copy(char *dst, const char *src, int maxn)
{
    int i;
    for (i = 0; i < maxn - 1 && src[i]; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

static const char *path_base(const char *p)
{
    const char *s = p;
    if (p == NULL) {
        return "";
    }
    for (; *p; p++) {
        if (*p == '\\' || *p == '/') {
            s = p + 1;
        }
    }
    return s;
}

static int load_syx_file(const char *path)
{
    ag_handle_t h;
    int64_t sz;
    uint8_t *buf;
    size_t got = 0;
    int n;

    if (path == NULL || path[0] == '\0') {
        return -1;
    }
    h = ag_open(path, AG_O_RDONLY);
    if (h < 0) {
        ag_printf("dx7: open failed: %s\n", path);
        return -1;
    }
    sz = ag_seek(h, 0, AG_SEEK_END);
    ag_seek(h, 0, AG_SEEK_SET);
    if (sz < 128 || sz > 64 * 1024) {
        ag_printf("dx7: bad syx size %d\n", (int)sz);
        ag_close(h);
        return -1;
    }
    buf = (uint8_t *)ag_malloc((size_t)sz);
    if (buf == NULL) {
        ag_close(h);
        return -1;
    }
    while (got < (size_t)sz) {
        int32_t r = ag_read(h, buf + got, (size_t)sz - got);
        if (r <= 0) {
            break;
        }
        got += (size_t)r;
    }
    ag_close(h);
    n = ag_dx7_syx_parse_bank(buf, (int)got, s_bank, AG_DX7_BANK_VOICES);
    ag_free(buf);
    if (n < 1) {
        ag_printf("dx7: no voices in %s\n", path);
        return -1;
    }
    s_bank_n = n;
    s_bank_voice = 0;
    s_use_bank = 1;
    path_copy(s_bank_path, path, SYX_PATH_LEN);
    if (ag_dx7_load_packed_voice(&s_dx, s_bank[0]) != 0) {
        return -1;
    }
    piano_all_off();
    s_dirty = 1;
    ag_printf("dx7: loaded %d voices from %s\n", s_bank_n, path);
    return 0;
}

static void select_bank_voice(int idx)
{
    if (s_bank_n < 1) {
        return;
    }
    if (idx < 0) {
        idx = s_bank_n - 1;
    }
    if (idx >= s_bank_n) {
        idx = 0;
    }
    s_bank_voice = idx;
    s_use_bank = 1;
    (void)ag_dx7_load_packed_voice(&s_dx, s_bank[s_bank_voice]);
    piano_all_off();
    s_dirty = 1;
}

static void scan_syx_dir(const char *dir)
{
    ag_handle_t d;
    ag_dirent_t ent;
    if (dir == NULL || s_syx_n >= SYX_MAX) {
        return;
    }
    d = ag_opendir(dir);
    if (d < 0) {
        return;
    }
    while (ag_readdir(d, &ent) == AG_OK && s_syx_n < SYX_MAX) {
        char full[SYX_PATH_LEN];
        int i, j;
        if (!ends_with_syx(ent.name)) {
            continue;
        }
        /* dir + \\ + name */
        for (i = 0; i < SYX_PATH_LEN - 1 && dir[i]; i++) {
            full[i] = dir[i];
        }
        if (i > 0 && full[i - 1] != '\\' && full[i - 1] != '/') {
            if (i < SYX_PATH_LEN - 1) {
                full[i++] = '\\';
            }
        }
        for (j = 0; i < SYX_PATH_LEN - 1 && ent.name[j]; j++, i++) {
            full[i] = ent.name[j];
        }
        full[i] = '\0';
        path_copy(s_syx_list[s_syx_n], full, SYX_PATH_LEN);
        s_syx_n++;
    }
    ag_close(d);
}

static void syx_next_file(int delta)
{
    if (s_syx_n < 1) {
        scan_syx_dir("h:");
        scan_syx_dir("h:\\dx7");
        scan_syx_dir(".");
    }
    if (s_syx_n < 1) {
        return;
    }
    s_syx_i += delta;
    if (s_syx_i < 0) {
        s_syx_i = s_syx_n - 1;
    }
    if (s_syx_i >= s_syx_n) {
        s_syx_i = 0;
    }
    (void)load_syx_file(s_syx_list[s_syx_i]);
}

static int fx_ensure(void)
{
    if (s_fx_ready) {
        return 0;
    }
    if (ag_fx_init(&s_fx, RATE) != 0) {
        ag_printf("dx7: FX init failed (OOM)\n");
        return -1;
    }
    s_fx_ready = 1;
    return 0;
}

static void fx_worker(void *arg)
{
    (void)arg;
    while (s_fx_run) {
        int slot = 0;
        if (!ag_queue_recv(s_q_dry, &slot, 50)) {
            continue;
        }
        if (slot < 0) {
            break;
        }
        if (slot > 1) {
            slot = 0;
        }
        memcpy(s_fx_wet[slot], s_fx_dry[slot], sizeof(s_fx_dry[0]));
        if (s_fx_ready) {
            ag_fx_process(&s_fx, s_fx_wet[slot], (int32_t)CHUNK);
        }
        (void)ag_queue_send(s_q_wet, &slot, 50);
    }
    ag_thread_exit();
}

static void fx_stop_worker(void)
{
    int poison = -1;
    if (s_fx_thr == NULL && s_q_dry == NULL) {
        s_fx_run = 0;
        s_fx_primed = 0;
        return;
    }
    s_fx_run = 0;
    if (s_q_dry) {
        (void)ag_queue_send(s_q_dry, &poison, 20);
    }
    if (s_fx_thr) {
        (void)ag_thread_join(s_fx_thr, 500);
        s_fx_thr = NULL;
    }
    if (s_q_dry) {
        ag_queue_delete(s_q_dry);
        s_q_dry = NULL;
    }
    if (s_q_wet) {
        ag_queue_delete(s_q_wet);
        s_q_wet = NULL;
    }
    s_fx_primed = 0;
    s_fx_slot = 0;
}

static int fx_start_worker(void)
{
    if (s_fx_thr != NULL) {
        return 0;
    }
    s_q_dry = ag_queue_create(2, sizeof(int));
    s_q_wet = ag_queue_create(2, sizeof(int));
    if (s_q_dry == NULL || s_q_wet == NULL) {
        fx_stop_worker();
        return -1;
    }
    s_fx_run = 1;
    s_fx_primed = 0;
    s_fx_slot = 0;
    /* prio a bit above typical app (0); run on system core */
    s_fx_thr = ag_thread_create(fx_worker, NULL, "dx7fx", 10 * 1024, 2,
                                AG_THREAD_SYS_CORE);
    if (s_fx_thr == NULL) {
        fx_stop_worker();
        return -1;
    }
    return 0;
}

static int fx_set_mode(int mode)
{
    if (mode < FX_OFF) {
        mode = FX_OFF;
    }
    if (mode > FX_CORE1) {
        mode = FX_OFF;
    }
    if (mode == s_fx_mode) {
        return 0;
    }
    if (mode == FX_OFF) {
        fx_stop_worker();
        s_fx_mode = FX_OFF;
        s_dirty = 1;
        return 0;
    }
    if (fx_ensure() != 0) {
        return -1;
    }
    if (mode == FX_LOCAL) {
        fx_stop_worker();
        s_fx_mode = FX_LOCAL;
        s_dirty = 1;
        return 0;
    }
    /* FX_CORE1 */
    fx_stop_worker();
    if (fx_start_worker() != 0) {
        ag_printf("dx7: FX core1 worker failed; staying local\n");
        s_fx_mode = FX_LOCAL;
        s_dirty = 1;
        return -1;
    }
    s_fx_mode = FX_CORE1;
    s_dirty = 1;
    return 0;
}

static const char *fx_mode_name(int mode)
{
    if (mode == FX_LOCAL) {
        return "local";
    }
    if (mode == FX_CORE1) {
        return "core1";
    }
    return "off";
}

static int parse_sink_arg(const char *arg)
{
    if (arg == NULL || arg[0] == '\0') {
        return 0;
    }
    if (str_ieq(arg, "nofx") || str_ieq(arg, "fxoff")) {
        s_fx_want = FX_OFF;
        return 0;
    }
    if (str_ieq(arg, "fx0") || str_ieq(arg, "fxlocal")) {
        s_fx_want = FX_LOCAL;
        return 0;
    }
    if (str_ieq(arg, "fx1") || str_ieq(arg, "fxcore") ||
        str_ieq(arg, "fxcore1")) {
        s_fx_want = FX_CORE1;
        return 0;
    }
    if (str_ieq(arg, "midivirt") || str_ieq(arg, "midi")) {
        s_midi_want = 1;
        return 0;
    }
    if (str_ieq(arg, "nomidi") || str_ieq(arg, "nomidivirt")) {
        s_midi_want = 0;
        return 0;
    }
    if (str_ieq(arg, "net") || str_ieq(arg, "tcp") || str_ieq(arg, "pcmvirt") ||
        str_ieq(arg, "mix") || str_ieq(arg, "pcmmix") ||
        str_ieq(arg, "audio") || str_ieq(arg, "i2s") || str_ieq(arg, "pcm0") ||
        str_ieq(arg, "mock") || str_ieq(arg, "mute") || str_ieq(arg, "pcmnull") ||
        ((arg[0] == 'n' || arg[0] == 'N') && (arg[1] == 'e' || arg[1] == 'E') &&
         (arg[2] == 't' || arg[2] == 'T') && arg[3] == ':') ||
        (arg[0] == '/' ||
         ((arg[0] == 'd' || arg[0] == 'D') && arg[1] == ':') ||
         ((arg[0] == 'p' || arg[0] == 'P') && (arg[1] == 'c' || arg[1] == 'C')))) {
        (void)ag_audio_out_resolve(arg, s_audio_path, sizeof(s_audio_path));
        return 0;
    }
    if (ends_with_syx(arg)) {
        path_copy(s_bank_path, arg, SYX_PATH_LEN);
        return 0;
    }
    if (ends_with_mid(arg)) {
        path_copy(s_mid_path, arg, SYX_PATH_LEN);
        return 0;
    }
    return -1;
}

static void held_clear_all(void)
{
    memset(s_note_held, 0, sizeof(s_note_held));
    s_held_n = 0;
}

static void held_set(uint8_t note, int on)
{
    if (note >= 128u) {
        return;
    }
    if (on) {
        if (!s_note_held[note]) {
            s_note_held[note] = 1;
            s_held_n++;
        }
    } else if (s_note_held[note]) {
        s_note_held[note] = 0;
        if (s_held_n > 0) {
            s_held_n--;
        }
    }
}

static void voice_note_on(uint8_t note, uint8_t vel)
{
    ag_dx7_note_on(&s_dx, note, vel ? vel : 100);
    held_set(note, 1);
    s_last_note = note;
    /* Do not s_dirty: full console redraw must not steal the audio budget. */
}

static void voice_note_off(uint8_t note)
{
    ag_dx7_note_off(&s_dx, note);
    held_set(note, 0);
}

static void voice_panic(void)
{
    ag_dx7_note_off_all(&s_dx);
    held_clear_all();
}

static void mid_note_cb(void *ctx, int on, uint8_t note, uint8_t vel,
                        uint8_t ch)
{
    (void)ctx;
    if (note == 0xffu) {
        voice_panic();
        return;
    }
    if (ch == 9u) {
        return; /* GM drum channel */
    }
    if (on) {
        voice_note_on(note, vel ? vel : 100);
        s_mid_notes++;
    } else {
        voice_note_off(note);
    }
}

static void open_midivirt(void)
{
    if (!s_midi_want || s_midi_fd >= 0) {
        return;
    }
    s_midi_fd = ag_dev_open("/dev/midivirt");
    if (s_midi_fd < 0) {
        /* Quiet: driver may not be installed yet. */
        return;
    }
    ag_printf("dx7: MIDI-in = /dev/midivirt (host midikbd.py :5559)\n");
}

static void close_midivirt(void)
{
    if (s_midi_fd >= 0) {
        (void)ag_dev_close(s_midi_fd);
        s_midi_fd = -1;
    }
}

/* Host midikbd --probe: F0 7D <note> F7 after note-on is inside DX7. */
static void midivirt_ack_note(uint8_t note)
{
    uint8_t ack[4];

    if (s_midi_fd < 0) {
        return;
    }
    ack[0] = 0xf0u;
    ack[1] = 0x7du;
    ack[2] = (uint8_t)(note & 0x7fu);
    ack[3] = 0xf7u;
    (void)ag_dev_write(s_midi_fd, ack, sizeof(ack));
}

static void pump_midivirt(void)
{
    uint8_t buf[64];
    int32_t n;
    int i;

    if (s_midi_fd < 0) {
        return;
    }
    n = ag_dev_read(s_midi_fd, buf, sizeof(buf));
    if (n <= 0) {
        return;
    }
    n = (n / 4) * 4;
    for (i = 0; i + 3 < n; i += 4) {
        uint8_t st = buf[i];
        uint8_t d1 = buf[i + 1];
        uint8_t d2 = buf[i + 2];
        uint8_t hi = (uint8_t)(st & 0xf0u);

        s_midi_ev++;
        if (hi == 0x90u) {
            if (d2) {
                voice_note_on(d1, d2);
                midivirt_ack_note(d1);
            } else {
                voice_note_off(d1);
            }
        } else if (hi == 0x80u) {
            voice_note_off(d1);
        } else if (hi == 0xb0u && d1 == 0x7bu) {
            voice_panic();
            {
                int k;
                for (k = 0; k < K_KEYS_N; k++) {
                    s_key_held[k] = 0;
                }
                s_notes_down = 0;
            }
        }
    }
}

static void mid_prog_cb(void *ctx, uint8_t prog, uint8_t ch)
{
    (void)ctx;
    if (ch == 9u || s_bank_n < 1) {
        return;
    }
    select_bank_voice((int)prog % s_bank_n);
}

static void mid_set_err(const char *msg)
{
    int i;
    for (i = 0; i < (int)sizeof(s_mid_err) - 1 && msg[i]; i++) {
        s_mid_err[i] = msg[i];
    }
    s_mid_err[i] = '\0';
}

static int load_mid_file(const char *path)
{
    ag_handle_t h;
    int64_t sz;
    uint8_t *buf;
    size_t got = 0;

    s_mid_err[0] = '\0';
    if (path == NULL || path[0] == '\0') {
        mid_set_err("empty path");
        return -1;
    }
    h = ag_open(path, AG_O_RDONLY);
    if (h < 0) {
        mid_set_err("open failed");
        ag_printf("dx7: mid open failed: %s\n", path);
        return -1;
    }
    sz = ag_seek(h, 0, AG_SEEK_END);
    ag_seek(h, 0, AG_SEEK_SET);
    if (sz < 14 || sz > 64 * 1024) {
        mid_set_err("bad size");
        ag_printf("dx7: bad mid size %d\n", (int)sz);
        ag_close(h);
        return -1;
    }
    buf = (uint8_t *)ag_malloc((size_t)sz);
    if (buf == NULL) {
        mid_set_err("oom file");
        ag_close(h);
        return -1;
    }
    while (got < (size_t)sz) {
        int32_t r = ag_read(h, buf + got, (size_t)sz - got);
        if (r <= 0) {
            break;
        }
        got += (size_t)r;
    }
    ag_close(h);
    if (got < 14u) {
        mid_set_err("short read");
        ag_free(buf);
        return -1;
    }
    if (ag_mid_load(&s_mid, buf, (int)got, path_base(path)) != 0) {
        mid_set_err("parse/oom");
        ag_printf("dx7: mid parse failed: %s (%u bytes)\n", path,
                  (unsigned)got);
        ag_free(buf);
        s_mid_loaded = 0;
        s_dirty = 1;
        return -1;
    }
    ag_free(buf);
    path_copy(s_mid_path, path, SYX_PATH_LEN);
    s_mid_loaded = 1;
    s_mid_notes = 0;
    s_mid_err[0] = '\0';
    ag_mid_set_loop(&s_mid, 1);
    ag_mid_start(&s_mid);
    ag_printf("dx7: MIDI %s — %d events, loop on\n", path, s_mid.nev);
    s_dirty = 1;
    return 0;
}

/* SMF only when argv passes path.mid — no HostFS autoload / autoplay. */
static void load_mid_if_requested(void)
{
    if (s_mid_path[0] != '\0') {
        (void)load_mid_file(s_mid_path);
    }
}

static int open_sink(void)
{
    if (s_audio_path[0] == '\0') {
        (void)ag_audio_out_resolve("pcmnull", s_audio_path, sizeof(s_audio_path));
    }
    s_audio_fd = ag_audio_out_open_dev(s_audio_path, RATE, 2);
    if (s_audio_fd < 0) {
        ag_printf("dx7: %s: %s\n", s_audio_path,
                  ag_strerror((ag_err_t)s_audio_fd));
        (void)ag_audio_out_resolve("pcmnull", s_audio_path, sizeof(s_audio_path));
        s_audio_fd = ag_audio_out_open_dev(s_audio_path, RATE, 2);
        if (s_audio_fd < 0) {
            return -1;
        }
    }
    ag_printf("dx7: sound = %s @ %u Hz\n", s_audio_path, (unsigned)RATE);
    return 0;
}

static void close_sink(void)
{
    if (s_audio_fd >= 0) {
        (void)ag_dev_close(s_audio_fd);
        s_audio_fd = -1;
    }
}

static int piano_index(int key)
{
    int i;
    for (i = 0; i < K_KEYS_N; i++) {
        if (k_keys[i].key == key) {
            return i;
        }
    }
    return -1;
}

/* Map typed char / Cyrillic (ЙЦУКЕН) → piano row when keycode is missing. */
static int piano_index_unicode(uint32_t uni)
{
    if (uni >= 'a' && uni <= 'z') {
        return piano_index((int)(AG_KEY_A + (uni - 'a')));
    }
    if (uni >= 'A' && uni <= 'Z') {
        return piano_index((int)(AG_KEY_A + (uni - 'A')));
    }
    if (uni >= '1' && uni <= '9') {
        return piano_index((int)(AG_KEY_1 + (uni - '1')));
    }
    if (uni == '0') {
        return piano_index(AG_KEY_0);
    }
    /* Physical QWERTY Z..M / S D G H J and Q..I on a Russian layout. */
    switch (uni) {
    case 0x044f: case 0x042f: return piano_index(AG_KEY_Z); /* я */
    case 0x0447: case 0x0427: return piano_index(AG_KEY_X); /* ч */
    case 0x0441: case 0x0421: return piano_index(AG_KEY_C); /* с */
    case 0x043c: case 0x041c: return piano_index(AG_KEY_V); /* м */
    case 0x0438: case 0x0418: return piano_index(AG_KEY_B); /* и */
    case 0x0442: case 0x0422: return piano_index(AG_KEY_N); /* т */
    case 0x044c: case 0x042c: return piano_index(AG_KEY_M); /* ь */
    case 0x044b: case 0x042b: return piano_index(AG_KEY_S); /* ы */
    case 0x0432: case 0x0412: return piano_index(AG_KEY_D); /* в */
    case 0x043f: case 0x041f: return piano_index(AG_KEY_G); /* п */
    case 0x0440: case 0x0420: return piano_index(AG_KEY_H); /* р */
    case 0x043e: case 0x041e: return piano_index(AG_KEY_J); /* о */
    case 0x0439: case 0x0419: return piano_index(AG_KEY_Q); /* й */
    case 0x0446: case 0x0426: return piano_index(AG_KEY_W); /* ц */
    case 0x0443: case 0x0423: return piano_index(AG_KEY_E); /* у */
    case 0x043a: case 0x041a: return piano_index(AG_KEY_R); /* к */
    case 0x0435: case 0x0415: return piano_index(AG_KEY_T); /* е */
    case 0x043d: case 0x041d: return piano_index(AG_KEY_Y); /* н */
    case 0x0433: case 0x0413: return piano_index(AG_KEY_U); /* г */
    case 0x0448: case 0x0428: return piano_index(AG_KEY_I); /* ш */
    default:
        return -1;
    }
}

static int piano_index_ex(int key, uint32_t uni)
{
    int pi = piano_index(key);
    if (pi >= 0) {
        return pi;
    }
    return piano_index_unicode(uni);
}

/* Ask host terminal for physical key up/down (poly chords). */
static void console_enable_key_events(void)
{
    /*
     * Kitty progressive enhancement + Windows Terminal win32-input-mode.
     * Ignored by classic cmd/PuTTY; then we fall back to unicode map + mono.
     */
    ag_printf("\x1b[>3u");
    ag_printf("\x1b[?9001h");
}

static void console_disable_key_events(void)
{
    ag_printf("\x1b[?9001l");
    ag_printf("\x1b[<u");
}

static void recount_keys(void)
{
    int i;
    int n = 0;
    for (i = 0; i < K_KEYS_N; i++) {
        if (s_key_held[i]) {
            n++;
        }
    }
    s_notes_down = n;
}

static void piano_all_off(void)
{
    int i;
    for (i = 0; i < K_KEYS_N; i++) {
        s_key_held[i] = 0;
    }
    s_notes_down = 0;
    voice_panic();
}

static void piano_release_all_held(void)
{
    int i;
    for (i = 0; i < K_KEYS_N; i++) {
        if (s_key_held[i]) {
            voice_note_off((uint8_t)k_keys[i].note);
            s_key_held[i] = 0;
        }
    }
    recount_keys();
}

static const char *note_name(uint8_t note)
{
    static const char *n[12] = {"C",  "C#", "D",  "D#", "E",  "F",
                                "F#", "G",  "G#", "A",  "A#", "B"};
    static char buf[8];
    int pc = (int)note % 12;
    int oct = (int)note / 12 - 1;
    /* small itoa */
    buf[0] = n[pc][0];
    if (n[pc][1]) {
        buf[1] = n[pc][1];
        buf[2] = (char)('0' + oct);
        buf[3] = '\0';
    } else {
        buf[1] = (char)('0' + oct);
        buf[2] = '\0';
    }
    return buf;
}

static const char *sink_name(void)
{
    return s_audio_path[0] ? s_audio_path : "/dev/pcmnull";
}

/* Build "C4 E4 G4" (up to 8 names) into out; uses note_name() carefully. */
static void format_held(char *out, int out_len)
{
    int i;
    int n = 0;
    int pos = 0;

    if (out == NULL || out_len < 2) {
        return;
    }
    out[0] = '\0';
    if (s_held_n <= 0) {
        out[0] = '-';
        out[1] = '\0';
        return;
    }
    for (i = 0; i < 128 && n < 8; i++) {
        const char *nm;
        int k;
        if (!s_note_held[i]) {
            continue;
        }
        nm = note_name((uint8_t)i);
        if (pos > 0 && pos + 1 < out_len) {
            out[pos++] = ' ';
        }
        for (k = 0; nm[k] && pos + 1 < out_len; k++) {
            out[pos++] = nm[k];
        }
        out[pos] = '\0';
        n++;
    }
    if (s_held_n > 8 && pos + 4 < out_len) {
        out[pos++] = ' ';
        out[pos++] = '.';
        out[pos++] = '.';
        out[pos++] = '.';
        out[pos] = '\0';
    }
}

static void draw_ui(void)
{
    const ag_dx7_patch_t *p = &s_dx.patch;
    uint32_t budget = CHUNK_US ? CHUNK_US : 1u;
    /* Home + EL; avoid full CLS (serial/QEMU — tens of ms, steals audio). */
    ag_printf("\x1b[H");
    ag_printf("DX7 structural FM  (%d-voice)  sink=%s\x1b[K\n", AG_DX7_VOICES,
              sink_name());
    ag_printf("-------------------------\x1b[K\n");
    if (s_use_bank && s_bank_n > 0) {
        ag_printf("Patch : bank %d/%d %-10s  (%s)\x1b[K\n", s_bank_voice + 1,
                  s_bank_n, p->name, path_base(s_bank_path));
    } else {
        ag_printf("Patch : builtin [%d] %-10s\x1b[K\n", s_preset, p->name);
    }
    if (s_mid_loaded) {
        ag_printf("MIDI  : %s  %s  ev %d/%d  noteons %u  loop\x1b[K\n",
                  s_mid.name, s_mid.playing ? "PLAY" : "stop", s_mid.iev,
                  s_mid.nev, (unsigned)s_mid_notes);
    } else if (s_mid_err[0]) {
        ag_printf("MIDI  : FAIL %s  (%s)\x1b[K\n", s_mid_err,
                  s_mid_path[0] ? s_mid_path : "?");
    } else {
        ag_printf("MIDI  : (none — pass path.mid to load/play)\x1b[K\n");
    }
    ag_printf("Alg   : %2u / 32     Feedback: %u     Transpose: %u (%+d st)\x1b[K\n",
              (unsigned)p->algorithm + 1u, (unsigned)p->feedback,
              (unsigned)p->transpose, (int)p->transpose - 24);
    ag_printf("OP lvl: %2u %2u %2u %2u %2u %2u\x1b[K\n",
              p->op[0].out_level, p->op[1].out_level, p->op[2].out_level,
              p->op[3].out_level, p->op[4].out_level, p->op[5].out_level);
    ag_printf("OP en : %c%c%c%c%c%c   car mute:%s\x1b[K\n",
              (s_dx.perf.op_enable & 1) ? '1' : '-',
              (s_dx.perf.op_enable & 2) ? '2' : '-',
              (s_dx.perf.op_enable & 4) ? '3' : '-',
              (s_dx.perf.op_enable & 8) ? '4' : '-',
              (s_dx.perf.op_enable & 16) ? '5' : '-',
              (s_dx.perf.op_enable & 32) ? '6' : '-',
              s_dx.perf.carrier_mute ? "ON" : "off");
    ag_printf("LFO   : spd %u  pmd %u  amd %u  wave %u\x1b[K\n",
              p->lfo_speed, p->lfo_pmd, p->lfo_amd, p->lfo_wave);
    ag_printf("Ctrl  : MW %u  AT %u  porta %s/%u  uni %u/%u  audition:%s\x1b[K\n",
              (unsigned)s_dx.perf.mod_wheel, (unsigned)s_dx.perf.aftertouch,
              s_dx.perf.porta_on ? "on" : "off",
              (unsigned)s_dx.perf.porta_time, (unsigned)s_dx.perf.unison,
              (unsigned)s_dx.perf.unison_detune,
              s_dx.perf.audition ? "ON" : "off");
    {
        char held[48];
        char lastbuf[8];
        const char *ln = note_name(s_last_note);
        int k;
        format_held(held, (int)sizeof(held));
        for (k = 0; ln[k] && k < 7; k++) {
            lastbuf[k] = ln[k];
        }
        lastbuf[k] = '\0';
        ag_printf("Voices: %d / %d active   kbdkeys: %d   keyup:%s\x1b[K\n",
                  ag_dx7_active_voices(&s_dx), AG_DX7_VOICES, s_notes_down,
                  s_have_keyup ? "yes(poly)" : "no(mono/VT)");
        ag_printf("Keys  : held %s   last %s(%u)   src %s   n=%d  inev %u\x1b[K\n",
                  held, lastbuf, (unsigned)s_last_note,
                  s_midi_fd >= 0 ? "midivirt+kbd" : "kbd", s_held_n,
                  (unsigned)s_midi_ev);
    }
    ag_printf("Perf  : render %u us / %u us (%u%%)  send %u us  loop %u us\x1b[K\n",
              (unsigned)s_render_us, (unsigned)budget, (unsigned)s_load_pct,
              (unsigned)s_send_us, (unsigned)s_loop_us);
    if (s_pcm_stats_ok) {
        ag_printf(
            "Stream: late %u  drop %u B  resync %u  chunk %u  "
            "tcp_ov %llu B eagain %u ring %u\x1b[K\n",
            (unsigned)s_late, (unsigned)s_drop, (unsigned)s_resync,
            (unsigned)CHUNK,
            (unsigned long long)s_pcm_stats.bytes_drop_overflow,
            (unsigned)s_pcm_stats.eagain_events,
            (unsigned)s_pcm_stats.ring_used);
    } else {
        ag_printf("Stream: late %u  drop %u B  resync %u  chunk %u\x1b[K\n",
                  (unsigned)s_late, (unsigned)s_drop, (unsigned)s_resync,
                  (unsigned)CHUNK);
    }
    if (s_fx_mode == FX_OFF) {
        ag_printf("FX    : mode=off  (F cycle  nofx|fx0|fx1)\x1b[K\n");
    } else {
        unsigned en = s_fx_ready ? s_fx.enable : 0u;
        ag_printf("FX    : mode=%s  D%c C%c R%c  wet %u/127  fxlate %u\x1b[K\n",
                  fx_mode_name(s_fx_mode),
                  (en & AG_FX_DELAY) ? '+' : '-',
                  (en & AG_FX_CHORUS) ? '+' : '-',
                  (en & AG_FX_REVERB) ? '+' : '-',
                  s_fx_ready ? (unsigned)s_fx.master_wet : 0u,
                  (unsigned)s_fx_late);
    }
    ag_printf("\x1b[K\n");
    ag_printf("Help  : , . patch   K L bank   Tab .syx   ; builtins\x1b[K\n");
    ag_printf("        Enter mid play/stop   \\ restart   A audition\x1b[K\n");
    ag_printf("        [ ] alg  - = fb   Space panic   Esc quit\x1b[K\n");
    ag_printf("        F FX mode   D/C/V delay/chor/rev   8/9 wet\x1b[K\n");
    ag_printf("        host midikbd.py → /dev/midivirt (poly)\x1b[K\n");
    ag_printf("\x1b[J");
    s_dirty = 0;
}

static void load_preset(int idx)
{
    if (idx < 0) {
        idx = AG_DX7_NPRESETS - 1;
    }
    if (idx >= AG_DX7_NPRESETS) {
        idx = 0;
    }
    s_preset = idx;
    s_use_bank = 0;
    ag_dx7_load_patch(&s_dx, ag_dx7_preset(s_preset));
    piano_all_off();
    s_dirty = 1;
}

static void handle_key(int key, int down, int is_repeat, uint32_t uni)
{
    int pi;

    /* FX controls (F / 8 / 9 always; D/C/V steal piano keys while FX on). */
    if (down && !is_repeat) {
        if (key == AG_KEY_F) {
            int next = s_fx_mode + 1;
            if (next > FX_CORE1) {
                next = FX_OFF;
            }
            (void)fx_set_mode(next);
            return;
        }
        if (key == AG_KEY_8) {
            if (s_fx_mode != FX_OFF && fx_ensure() == 0) {
                if (s_fx.master_wet >= 8u) {
                    s_fx.master_wet = (uint8_t)(s_fx.master_wet - 8u);
                } else {
                    s_fx.master_wet = 0;
                }
                s_dirty = 1;
            }
            return;
        }
        if (key == AG_KEY_9) {
            if (s_fx_mode != FX_OFF && fx_ensure() == 0) {
                if (s_fx.master_wet <= 119u) {
                    s_fx.master_wet = (uint8_t)(s_fx.master_wet + 8u);
                } else {
                    s_fx.master_wet = 127u;
                }
                s_dirty = 1;
            }
            return;
        }
        if (s_fx_mode != FX_OFF && fx_ensure() == 0) {
            if (key == AG_KEY_D) {
                s_fx.enable ^= AG_FX_DELAY;
                s_dirty = 1;
                return;
            }
            if (key == AG_KEY_C) {
                s_fx.enable ^= AG_FX_CHORUS;
                s_dirty = 1;
                return;
            }
            if (key == AG_KEY_V) {
                s_fx.enable ^= AG_FX_REVERB;
                s_dirty = 1;
                return;
            }
        }
    }

    pi = piano_index_ex(key, uni);
    if (pi >= 0) {
        uint8_t note = (uint8_t)k_keys[pi].note;
        /*
         * With win32-input / kitty we get real KEY_UP → polyphonic hold.
         * Plain VT (no releases): each new press frees previous keys so pitch
         * changes; unicode map covers RU layout when keycode is AG_KEY_NONE.
         */
        if (down) {
            if (is_repeat && s_have_keyup) {
                return;
            }
            if (!s_have_keyup) {
                piano_release_all_held();
            }
            if (!s_key_held[pi]) {
                voice_note_on(note, 100);
                s_key_held[pi] = 1;
                recount_keys();
            }
        } else {
            s_have_keyup = 1;
            if (s_key_held[pi]) {
                voice_note_off(note);
                s_key_held[pi] = 0;
                recount_keys();
            }
        }
        return;
    }
    if (!down) {
        return;
    }
    switch (key) {
    case AG_KEY_ESC:
        ag_exit(0);
        break;
    case AG_KEY_SPACE:
        piano_all_off();
        break;
    case AG_KEY_ENTER:
        if (!s_mid_loaded) {
            break;
        }
        if (s_mid.playing) {
            ag_mid_stop(&s_mid);
            voice_panic();
        } else {
            ag_mid_start(&s_mid);
        }
        s_dirty = 1;
        break;
    case AG_KEY_BACKSLASH:
        if (!s_mid_loaded) {
            break;
        }
        voice_panic();
        ag_mid_start(&s_mid);
        s_mid_notes = 0;
        s_dirty = 1;
        break;
    case AG_KEY_A:
        /* Not a piano white key in our map (A is not on Z..M / Q..I rows). */
        ag_dx7_set_audition(&s_dx, s_dx.perf.audition ? 0 : 1);
        s_dirty = 1;
        break;
    case AG_KEY_1:
    case AG_KEY_2:
    case AG_KEY_3:
    case AG_KEY_4:
    case AG_KEY_5:
    case AG_KEY_6:
        ag_dx7_toggle_op(&s_dx, key - AG_KEY_1);
        s_dirty = 1;
        break;
    case AG_KEY_0:
        s_dx.perf.op_enable = 0x3fu;
        ag_dx7_mute_carriers(&s_dx, 0);
        s_dirty = 1;
        break;
    case AG_KEY_APOSTROPHE:
        ag_dx7_mute_carriers(&s_dx, s_dx.perf.carrier_mute ? 0 : 1);
        s_dirty = 1;
        break;
    case AG_KEY_LEFT:
        ag_dx7_set_mod_wheel(&s_dx, (uint8_t)(s_dx.perf.mod_wheel > 8
                                                  ? s_dx.perf.mod_wheel - 8
                                                  : 0));
        s_dirty = 1;
        break;
    case AG_KEY_RIGHT:
        ag_dx7_set_mod_wheel(&s_dx, (uint8_t)(s_dx.perf.mod_wheel < 119
                                                  ? s_dx.perf.mod_wheel + 8
                                                  : 127));
        s_dirty = 1;
        break;
    case AG_KEY_DOWN:
        ag_dx7_set_aftertouch(&s_dx, (uint8_t)(s_dx.perf.aftertouch > 8
                                                   ? s_dx.perf.aftertouch - 8
                                                   : 0));
        s_dirty = 1;
        break;
    case AG_KEY_UP:
        ag_dx7_set_aftertouch(&s_dx, (uint8_t)(s_dx.perf.aftertouch < 119
                                                   ? s_dx.perf.aftertouch + 8
                                                   : 127));
        s_dirty = 1;
        break;
    case AG_KEY_P:
        if (s_dx.perf.porta_on) {
            ag_dx7_set_porta(&s_dx, 0, s_dx.perf.porta_time);
        } else {
            ag_dx7_set_porta(&s_dx, 1, s_dx.perf.porta_time ? s_dx.perf.porta_time
                                                            : 40);
        }
        s_dirty = 1;
        break;
    case AG_KEY_O: {
        uint8_t u = (uint8_t)(s_dx.perf.unison + 1u);
        if (u > 4u) {
            u = 1;
        }
        ag_dx7_set_unison(&s_dx, u, s_dx.perf.unison_detune ? s_dx.perf.unison_detune
                                                            : 25);
        s_dirty = 1;
        break;
    }
    case AG_KEY_LBRACKET:
        ag_dx7_set_algorithm(&s_dx, (uint8_t)((s_dx.patch.algorithm + 31) % 32));
        s_dirty = 1;
        break;
    case AG_KEY_RBRACKET:
        ag_dx7_set_algorithm(&s_dx, (uint8_t)((s_dx.patch.algorithm + 1) % 32));
        s_dirty = 1;
        break;
    case AG_KEY_MINUS:
        if (s_dx.patch.feedback > 0) {
            ag_dx7_set_feedback(&s_dx, (uint8_t)(s_dx.patch.feedback - 1));
        }
        s_dirty = 1;
        break;
    case AG_KEY_EQUAL:
        ag_dx7_set_feedback(&s_dx, (uint8_t)(s_dx.patch.feedback + 1));
        s_dirty = 1;
        break;
    case AG_KEY_COMMA:
        if (s_use_bank && s_bank_n > 0) {
            select_bank_voice(s_bank_voice - 1);
        } else {
            load_preset(s_preset - 1);
        }
        break;
    case AG_KEY_PERIOD:
        if (s_use_bank && s_bank_n > 0) {
            select_bank_voice(s_bank_voice + 1);
        } else {
            load_preset(s_preset + 1);
        }
        break;
    case AG_KEY_K:
        select_bank_voice(s_bank_voice - 1);
        break;
    case AG_KEY_L:
        select_bank_voice(s_bank_voice + 1);
        break;
    case AG_KEY_TAB:
        syx_next_file(1);
        break;
    case AG_KEY_SEMICOLON:
        load_preset(s_preset);
        break;
    case AG_KEY_F1:
    case AG_KEY_F2:
    case AG_KEY_F3:
    case AG_KEY_F4:
    case AG_KEY_F5:
    case AG_KEY_F6: {
        int op = key - AG_KEY_F1;
        int lvl = (int)s_dx.patch.op[op].out_level - 10;
        if (lvl < 0) {
            lvl = 0;
        }
        ag_dx7_set_op_level(&s_dx, op, (uint8_t)lvl);
        s_dirty = 1;
        break;
    }
    case AG_KEY_F7:
    case AG_KEY_F8:
    case AG_KEY_F9:
    case AG_KEY_F10:
    case AG_KEY_F11:
    case AG_KEY_F12: {
        int op = key - AG_KEY_F7;
        if (op > 5) {
            op = 5;
        }
        int lvl = (int)s_dx.patch.op[op].out_level + 10;
        if (lvl > 99) {
            lvl = 99;
        }
        ag_dx7_set_op_level(&s_dx, op, (uint8_t)lvl);
        s_dirty = 1;
        break;
    }
    default:
        break;
    }
}

static void pace_wait(ag_time_t due)
{
    for (;;) {
        ag_time_t now = ag_micros();
        uint32_t rem;
        if (now >= due) {
            return;
        }
        rem = (uint32_t)(due - now);
        if (rem > 2000u) {
            ag_delay((rem / 1000u) - 1u);
        } else {
            ag_heartbeat();
        }
    }
}

static int16_t s_fx_prev_wet[CHUNK * 2];

static void pump_audio(void)
{
    const int32_t n = (int32_t)CHUNK;
    const size_t bytes = (size_t)n * 4u;
    ag_time_t t0, t1;
    int32_t wr;
    int core1 = (s_fx_mode == FX_CORE1 && s_q_dry && s_q_wet);

    t0 = ag_micros();
    if (s_mid_loaded && s_mid.playing) {
        ag_mid_advance(&s_mid, (uint32_t)n, RATE, mid_note_cb, mid_prog_cb,
                       NULL);
    }
    ag_dx7_render(&s_dx, s_pcm, n);

    if (s_fx_mode == FX_LOCAL && s_fx_ready) {
        ag_fx_process(&s_fx, s_pcm, n);
    } else if (core1) {
        int slot = s_fx_slot;

        memcpy(s_fx_dry[slot], s_pcm, sizeof(s_fx_dry[0]));
        if (!ag_queue_send(s_q_dry, &slot, 5)) {
            s_fx_late++;
        } else {
            s_fx_slot ^= 1;
        }
        /* +1 chunk: play previous wet (or silence) while worker runs. */
        if (s_fx_primed) {
            memcpy(s_pcm, s_fx_prev_wet, sizeof(s_fx_prev_wet));
        } else {
            memset(s_pcm, 0, sizeof(s_pcm));
        }
    }

    t1 = ag_micros();
    s_render_us = (uint32_t)(t1 - t0);
    if (CHUNK_US > 0u) {
        s_load_pct = (s_render_us * 100u + CHUNK_US / 2u) / CHUNK_US;
        if (s_load_pct == 0u && s_render_us > 0u) {
            s_load_pct = 1u; /* show sub-percent as 1% */
        }
    }

    if (s_audio_fd < 0) {
        s_send_us = 0;
    } else {
        wr = ag_dev_write(s_audio_fd, s_pcm, bytes);
        s_send_us = (uint32_t)(ag_micros() - t1);
        if (wr < 0) {
            if (!s_send_err_reported) {
                s_send_err_reported = 1;
                ag_printf("dx7: %s: %s\n", s_audio_path,
                          ag_strerror((ag_err_t)wr));
            }
            s_drop += (uint32_t)bytes;
        } else if ((size_t)wr < bytes) {
            s_drop += (uint32_t)(bytes - (size_t)wr);
        }
    }

    /* Collect wet after write so FX overlaps device I/O. */
    if (core1) {
        int wet_slot = 0;
        if (ag_queue_recv(s_q_wet, &wet_slot, 10)) {
            if (wet_slot < 0 || wet_slot > 1) {
                wet_slot = 0;
            }
            memcpy(s_fx_prev_wet, s_fx_wet[wet_slot], sizeof(s_fx_prev_wet));
            s_fx_primed = 1;
        } else {
            s_fx_late++;
        }
    }
}

int ag_main(int argc, char **argv)
{
    ag_event_t ev;
    int i;

    (void)ag_audio_out_resolve("pcmnull", s_audio_path, sizeof(s_audio_path));

    for (i = 1; i < argc; i++) {
        if (parse_sink_arg(argv[i]) != 0) {
            ag_printf("dx7: unknown arg '%s' (pcmmix|pcmvirt|pcmnull|audio|mock|"
                      "nofx|fx0|fx1|midivirt|nomidi|.syx|.mid)\n",
                      argv[i]);
        }
    }

    if (open_sink() != 0) {
        return 1;
    }

    ag_dx7_init(&s_dx, RATE);
    ag_mid_init(&s_mid);
    open_midivirt();
    if (s_fx_want != FX_OFF) {
        (void)fx_set_mode(s_fx_want);
    }
    console_enable_key_events();
    load_preset(0);
    if (s_bank_path[0] != '\0') {
        if (load_syx_file(s_bank_path) != 0) {
            ag_printf("dx7: continuing with builtin presets\n");
        }
    } else {
        scan_syx_dir("h:");
        scan_syx_dir("h:\\dx7");
        if (s_syx_n > 0) {
            s_syx_i = 0;
            (void)load_syx_file(s_syx_list[0]);
        }
    }
    load_mid_if_requested();
    draw_ui();
    s_ui_ms = ag_millis();
    s_next_due = ag_micros() + (ag_time_t)CHUNK_US;

    for (;;) {
        ag_time_t loop0 = ag_micros();

        while (ag_poll_event(&ev, 0)) {
            if (ev.type == AG_EV_QUIT) {
                goto done;
            }
            if (ev.type == AG_EV_KEY_DOWN || ev.type == AG_EV_KEY_UP) {
                handle_key((int)ev.key.keycode, ev.type == AG_EV_KEY_DOWN,
                           ev.key.repeat ? 1 : 0, ev.key.unicode);
            }
        }

        pump_midivirt();
        pump_audio();

        /*
         * UI only in spare time before the audio due — never after pace_wait
         * (that stole the next chunk and spiked loop/late under note spam).
         * Note on/off do not set s_dirty; Keys/Perf refresh on the period.
         */
        {
            ag_time_t now = ag_micros();
            uint32_t ms = ag_millis();
            int want_ui = s_dirty || (ms - s_ui_ms) >= UI_PERIOD_MS;

            if (want_ui && now < s_next_due &&
                (uint32_t)(s_next_due - now) >= UI_SLACK_US) {
                s_ui_ms = ms;
                s_pcm_stats_ok = 0;
                if (s_audio_fd >= 0) {
                    ag_err_t st = ag_dev_ioctl(s_audio_fd, AG_IOC_AUDIO_GETSTATS,
                                               &s_pcm_stats, sizeof(s_pcm_stats));
                    s_pcm_stats_ok = (st == AG_OK) ? 1 : 0;
                }
                draw_ui();
                now = ag_micros();
            }

            /*
             * Steady realtime clock: never "catch up" by bursting extra chunks
             * (that floods pcmplay → overflow drop → underrun crackle on a sine).
             * If late, slide the schedule forward from now.
             */
            if (now <= s_next_due) {
                pace_wait(s_next_due);
                s_next_due += (ag_time_t)CHUNK_US;
            } else {
                s_late++;
                if (now > s_next_due + (ag_time_t)CHUNK_US) {
                    s_resync++;
                }
                s_next_due = now + (ag_time_t)CHUNK_US;
            }
        }

        s_loop_us = (uint32_t)(ag_micros() - loop0);
        ag_heartbeat();
    }

done:
    voice_panic();
    fx_stop_worker();
    if (s_fx_ready) {
        ag_fx_free(&s_fx);
        s_fx_ready = 0;
    }
    close_midivirt();
    ag_mid_unload(&s_mid);
    console_disable_key_events();
    close_sink();
    return 0;
}
