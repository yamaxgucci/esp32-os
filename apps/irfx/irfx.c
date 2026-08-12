/*
 * IRFX — impulse-response / convolution FX for ArgonOS.
 *
 * Partitioned FFT convolution, ≤500 ms mono IR @ 22.05 kHz.
 * Dry: clicks / noise bursts / optional WAV. Sink: pcmvirt|pcmnull|…
 *
 *   python tools/mkaxe.py --arch xtensa --gcc xtensa-esp32s3-elf-gcc `
 *     --include sdk/include --include apps/common --include apps/common/ir `
 *     --include apps/common/wav --include apps/common/libc `
 *     --cflags "-Os -ffunction-sections -fdata-sections -fno-builtin" `
 *     -o build/apps/IRFX.AXE `
 *     apps/irfx/irfx.c apps/common/ir/ag_ir.c apps/common/ir/ag_fft.c `
 *     apps/common/wav/ag_wav.c apps/common/libc/libc_shim.c
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>

#include <argon/argon.h>
#include <argon/keys.h>

#include "audio_out.h"
#include "ag_ir.h"
#include "ag_wav.h"

AG_APP_SIZED("IRFX", "0.2", "argon", 0, 12 * 1024, 1024 * 1024);

#define RATE  22050u
#define CHUNK AG_IR_BLOCK
#define CHUNK_US ((uint32_t)((CHUNK * 1000000ull) / RATE))
#define UI_PERIOD_MS 250u

enum {
    SRC_CLICK = 0,
    SRC_NOISE,
    SRC_WAV,
    SRC_N
};

static const char *const k_src_name[SRC_N] = { "click", "noise", "wav" };
static const char *const k_preset_name[3] = { "room", "hall", "spring" };

static ag_ir_t     s_ir;
static int16_t     s_mono[CHUNK];
static int16_t     s_stereo[CHUNK * 2];
static char        s_audio_path[AG_PATH_MAX];
static ag_handle_t s_audio_fd = -1;

static int  s_src = SRC_CLICK;
static int  s_preset = 1;
static int  s_running = 1;
static int  s_dirty = 1;
static int  s_auto_click = 1;
static uint32_t s_click_period = RATE * 3u / 2u; /* 1.5 s */
static uint32_t s_phase; /* samples since last auto trigger */
static uint32_t s_burst_left;
static uint32_t s_noise_left;
static uint32_t s_rng = 1u;

static ag_wav_pcm_t s_dry;
static uint32_t     s_dry_pos;
static char         s_ir_name[48] = "hall";
static char         s_dry_name[48] = "(none)";
static char         s_status[72] = "";

static char s_ir_path[AG_PATH_MAX];
static char s_dry_path[AG_PATH_MAX];

/* Resource / timing (refreshed every UI_PERIOD_MS). */
static uint32_t s_render_us;
static uint32_t s_send_us;
static uint32_t s_loop_us;
static uint32_t s_load_pct;
static uint32_t s_late;
static uint32_t s_ui_ms;
static ag_meminfo_t s_mem;
static ag_audio_stats_t s_pcm_stats;
static int s_pcm_stats_ok;

static int str_ieq(const char *a, const char *b)
{
    if (a == NULL || b == NULL) {
        return 0;
    }
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') {
            ca = (char)(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (char)(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static int ends_with_ci(const char *s, const char *suf)
{
    size_t n, m, i;
    if (s == NULL || suf == NULL) {
        return 0;
    }
    n = 0;
    while (s[n]) {
        n++;
    }
    m = 0;
    while (suf[m]) {
        m++;
    }
    if (n < m) {
        return 0;
    }
    for (i = 0; i < m; i++) {
        char ca = s[n - m + i], cb = suf[i];
        if (ca >= 'A' && ca <= 'Z') {
            ca = (char)(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (char)(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return 0;
        }
    }
    return 1;
}

static void set_status(const char *s)
{
    size_t i = 0;
    if (s == NULL) {
        s_status[0] = '\0';
        return;
    }
    while (s[i] && i + 1u < sizeof(s_status)) {
        s_status[i] = s[i];
        i++;
    }
    s_status[i] = '\0';
    s_dirty = 1;
}

static void path_base(char *out, size_t outlen, const char *path)
{
    const char *base = path;
    const char *p;
    size_t i = 0;
    if (path == NULL) {
        out[0] = '\0';
        return;
    }
    for (p = path; *p; p++) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
    }
    while (base[i] && i + 1u < outlen) {
        out[i] = base[i];
        i++;
    }
    out[i] = '\0';
}

static int16_t rnd16(void)
{
    s_rng = s_rng * 1664525u + 1013904223u;
    return (int16_t)((int32_t)(s_rng >> 16) - 32768);
}

static int load_ir_wav(const char *path)
{
    ag_wav_pcm_t w;
    if (ag_wav_load(path, &w) != 0) {
        set_status("IR wav load failed");
        return -1;
    }
    if (ag_ir_load(&s_ir, w.data, w.frames, w.rate) != 0) {
        ag_wav_free(&w);
        set_status("IR build failed");
        return -1;
    }
    ag_wav_free(&w);
    path_base(s_ir_name, sizeof(s_ir_name), path);
    set_status("IR loaded");
    return 0;
}

static int load_preset(int preset)
{
    if (ag_ir_load_preset(&s_ir, preset) != 0) {
        set_status("preset failed");
        return -1;
    }
    s_preset = preset;
    {
        size_t i = 0;
        const char *n = k_preset_name[preset];
        while (n[i] && i + 1u < sizeof(s_ir_name)) {
            s_ir_name[i] = n[i];
            i++;
        }
        s_ir_name[i] = '\0';
    }
    set_status("preset OK");
    return 0;
}

static int load_dry_wav(const char *path)
{
    ag_wav_free(&s_dry);
    s_dry_pos = 0;
    if (ag_wav_load(path, &s_dry) != 0) {
        set_status("dry wav failed");
        s_dry_name[0] = '\0';
        return -1;
    }
    path_base(s_dry_name, sizeof(s_dry_name), path);
    s_src = SRC_WAV;
    set_status("dry loaded");
    return 0;
}

static void trigger_click(void)
{
    s_burst_left = RATE / 200u; /* ~5 ms impulse-ish */
    if (s_burst_left < 4u) {
        s_burst_left = 4u;
    }
    s_phase = 0;
}

static void trigger_noise(void)
{
    s_noise_left = RATE / 5u; /* 200 ms */
    s_phase = 0;
}

static void fill_mono(void)
{
    uint32_t i;
    memset(s_mono, 0, sizeof(s_mono));

    if (s_src == SRC_WAV && s_dry.data != NULL && s_dry.frames > 0u) {
        for (i = 0; i < CHUNK; i++) {
            s_mono[i] = s_dry.data[s_dry_pos];
            s_dry_pos++;
            if (s_dry_pos >= s_dry.frames) {
                s_dry_pos = 0;
            }
        }
        return;
    }

    if (s_auto_click && s_src == SRC_CLICK) {
        s_phase += CHUNK;
        if (s_phase >= s_click_period) {
            s_phase = 0;
            trigger_click();
        }
    }

    if (s_burst_left > 0u) {
        for (i = 0; i < CHUNK && s_burst_left > 0u; i++) {
            /* decaying click */
            int32_t a = (int32_t)s_burst_left * 200;
            if (a > 28000) {
                a = 28000;
            }
            s_mono[i] = (int16_t)a;
            s_burst_left--;
        }
    }

    if (s_src == SRC_NOISE) {
        if (s_auto_click) {
            s_phase += CHUNK;
            if (s_phase >= s_click_period) {
                s_phase = 0;
                trigger_noise();
            }
        }
        if (s_noise_left > 0u) {
            for (i = 0; i < CHUNK; i++) {
                if (s_noise_left > 0u) {
                    s_mono[i] = (int16_t)(((int32_t)rnd16() * 3) >> 3);
                    s_noise_left--;
                }
            }
        }
    }
}

static int open_sink(void)
{
    if (s_audio_path[0] == '\0') {
        (void)ag_audio_out_resolve("pcmnull", s_audio_path, sizeof(s_audio_path));
    }
    s_audio_fd = ag_audio_out_open_dev(s_audio_path, RATE, 2);
    if (s_audio_fd < 0) {
        ag_printf("irfx: %s: %s\n", s_audio_path,
                  ag_strerror((ag_err_t)s_audio_fd));
        (void)ag_audio_out_resolve("pcmnull", s_audio_path, sizeof(s_audio_path));
        s_audio_fd = ag_audio_out_open_dev(s_audio_path, RATE, 2);
        if (s_audio_fd < 0) {
            return -1;
        }
    }
    ag_printf("irfx: sound = %s @ %u Hz\n", s_audio_path, (unsigned)RATE);
    return 0;
}

static uint32_t ir_spectra_kb(void)
{
    /* H + X: parts * FFT * 2 (re/im) * sizeof(int16_t) each */
    uint32_t bytes;
    if (!s_ir.ready || s_ir.parts == 0u) {
        return 0u;
    }
    bytes = s_ir.parts * AG_IR_FFT * 2u * (uint32_t)sizeof(int16_t) * 2u;
    return (bytes + 1023u) / 1024u;
}

static void draw_ui(void)
{
    uint32_t ms;
    uint32_t arena_used_kb, arena_total_kb, arena_free_kb;
    uint32_t dry_kb;

    if (!s_dirty) {
        return;
    }
    s_dirty = 0;
    ag_cls();
    ag_printf("IRFX — convolution FX (max %u ms)\n", (unsigned)AG_IR_MAX_MS);
    ag_printf("sink : %s\n", s_audio_path);
    ms = s_ir.ready ? (s_ir.ir_frames * 1000u / s_ir.rate) : 0u;
    ag_printf("IR   : %s  %u frames (~%u ms)  parts=%u\n", s_ir_name,
              (unsigned)s_ir.ir_frames, (unsigned)ms, (unsigned)s_ir.parts);
    ag_printf("dry  : %s  src=%s\n", s_dry_name[0] ? s_dry_name : "(synth)",
              k_src_name[s_src]);
    ag_printf("wet  : %u/127   gain=%u/127   bypass=%s   auto=%s\n",
              (unsigned)s_ir.wet, (unsigned)s_ir.gain, s_ir.bypass ? "ON" : "off",
              s_auto_click ? "on" : "off");
    ag_printf("stat : %s\n", s_status[0] ? s_status : "-");

    arena_total_kb = (uint32_t)(s_mem.arena_total / 1024u);
    arena_free_kb = (uint32_t)(s_mem.arena_free / 1024u);
    arena_used_kb = arena_total_kb > arena_free_kb ? arena_total_kb - arena_free_kb
                                                   : 0u;
    dry_kb = 0u;
    if (s_dry.data != NULL && s_dry.frames > 0u) {
        dry_kb = (uint32_t)((s_dry.frames * sizeof(int16_t) + 1023u) / 1024u);
    }
    ag_printf("CPU  : render %u us / %u us (%u%%)  send %u us  loop %u us  "
              "late %u\n",
              (unsigned)s_render_us, (unsigned)CHUNK_US, (unsigned)s_load_pct,
              (unsigned)s_send_us, (unsigned)s_loop_us, (unsigned)s_late);
    ag_printf("MEM  : arena %u / %u KB used  free %u KB  largest %u KB\n",
              (unsigned)arena_used_kb, (unsigned)arena_total_kb,
              (unsigned)arena_free_kb,
              (unsigned)(s_mem.arena_largest / 1024u));
    ag_printf("DSP  : IR spectra %u KB  dry wav %u KB  block %u  fft %u\n",
              (unsigned)ir_spectra_kb(), (unsigned)dry_kb, (unsigned)AG_IR_BLOCK,
              (unsigned)AG_IR_FFT);
    if (s_pcm_stats_ok) {
        ag_printf("PCM  : drop_ov %llu B  eagain %u  ring %u / %u\n",
                  (unsigned long long)s_pcm_stats.bytes_drop_overflow,
                  (unsigned)s_pcm_stats.eagain_events,
                  (unsigned)s_pcm_stats.ring_used,
                  (unsigned)s_pcm_stats.ring_cap);
    }

    ag_printf("\n");
    ag_printf("[Space] trigger   [A] auto   [B] bypass\n");
    ag_printf("[1/2/3] room/hall/spring   [S] click  [N] noise  [W] wav-src\n");
    ag_printf("[ / ] wet-/+    -/= gain-/+    Esc quit\n");
}

static void handle_key(int key, uint32_t uni)
{
    (void)uni;
    if (key == AG_KEY_ESC) {
        s_running = 0;
        return;
    }
    if (key == AG_KEY_SPACE) {
        if (s_src == SRC_NOISE) {
            trigger_noise();
        } else if (s_src != SRC_WAV) {
            trigger_click();
        }
        set_status("trigger");
        return;
    }
    if (key == AG_KEY_A) {
        s_auto_click = !s_auto_click;
        s_dirty = 1;
        return;
    }
    if (key == AG_KEY_B) {
        ag_ir_set_bypass(&s_ir, !s_ir.bypass);
        s_dirty = 1;
        return;
    }
    if (key == AG_KEY_1) {
        (void)load_preset(0);
        return;
    }
    if (key == AG_KEY_2) {
        (void)load_preset(1);
        return;
    }
    if (key == AG_KEY_3) {
        (void)load_preset(2);
        return;
    }
    if (key == AG_KEY_S) {
        s_src = SRC_CLICK;
        s_dirty = 1;
        return;
    }
    if (key == AG_KEY_N) {
        s_src = SRC_NOISE;
        s_dirty = 1;
        return;
    }
    if (key == AG_KEY_W) {
        if (s_dry.data != NULL) {
            s_src = SRC_WAV;
            s_dirty = 1;
        } else {
            set_status("no dry wav (pass path.wav)");
        }
        return;
    }
    if (key == AG_KEY_LBRACKET) {
        if (s_ir.wet > 4u) {
            ag_ir_set_wet(&s_ir, (uint8_t)(s_ir.wet - 4u));
        } else {
            ag_ir_set_wet(&s_ir, 0);
        }
        s_dirty = 1;
        return;
    }
    if (key == AG_KEY_RBRACKET) {
        if (s_ir.wet < 123u) {
            ag_ir_set_wet(&s_ir, (uint8_t)(s_ir.wet + 4u));
        } else {
            ag_ir_set_wet(&s_ir, 127);
        }
        s_dirty = 1;
        return;
    }
    if (key == AG_KEY_MINUS) {
        if (s_ir.gain > 4u) {
            ag_ir_set_gain(&s_ir, (uint8_t)(s_ir.gain - 4u));
        } else {
            ag_ir_set_gain(&s_ir, 0);
        }
        s_dirty = 1;
        return;
    }
    if (key == AG_KEY_EQUAL) {
        if (s_ir.gain < 123u) {
            ag_ir_set_gain(&s_ir, (uint8_t)(s_ir.gain + 4u));
        } else {
            ag_ir_set_gain(&s_ir, 127);
        }
        s_dirty = 1;
        return;
    }
}

static int parse_args(int argc, char **argv)
{
    int i;
    (void)ag_audio_out_resolve("pcmnull", s_audio_path, sizeof(s_audio_path));
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a == NULL || a[0] == '\0') {
            continue;
        }
        if (str_ieq(a, "pcmvirt") || str_ieq(a, "pcmnull") ||
            str_ieq(a, "pcmmix") || str_ieq(a, "mix") || str_ieq(a, "audio") ||
            str_ieq(a, "mock") || str_ieq(a, "net") || a[0] == '/' ||
            (a[0] == 'h' && a[1] == ':' && a[2] == '\\' &&
             !ends_with_ci(a, ".wav"))) {
            if (ag_audio_out_resolve(a, s_audio_path, sizeof(s_audio_path)) ==
                0) {
                continue;
            }
        }
        if (ends_with_ci(a, ".wav")) {
            /* First wav = IR, second = dry (or IR if path has "ir") */
            if (s_ir_path[0] == '\0') {
                size_t n = 0;
                while (a[n] && n + 1u < sizeof(s_ir_path)) {
                    s_ir_path[n] = a[n];
                    n++;
                }
                s_ir_path[n] = '\0';
            } else if (s_dry_path[0] == '\0') {
                size_t n = 0;
                while (a[n] && n + 1u < sizeof(s_dry_path)) {
                    s_dry_path[n] = a[n];
                    n++;
                }
                s_dry_path[n] = '\0';
            }
            continue;
        }
        if (str_ieq(a, "room")) {
            s_preset = 0;
        } else if (str_ieq(a, "hall")) {
            s_preset = 1;
        } else if (str_ieq(a, "spring")) {
            s_preset = 2;
        }
    }
    return 0;
}

int ag_main(int argc, char **argv)
{
    ag_time_t next_due;
    ag_event_t ev;

    (void)parse_args(argc, argv);

    if (ag_ir_init(&s_ir, RATE) != 0) {
        ag_printf("irfx: init failed\n");
        return 1;
    }

    if (s_ir_path[0] != '\0') {
        if (load_ir_wav(s_ir_path) != 0) {
            (void)load_preset(s_preset);
        }
    } else {
        (void)load_preset(s_preset);
    }
    if (s_dry_path[0] != '\0') {
        (void)load_dry_wav(s_dry_path);
    }

    if (open_sink() != 0) {
        ag_ir_free(&s_ir);
        return 1;
    }

    ag_cursor(false);
    ag_meminfo(&s_mem);
    s_dirty = 1;
    s_ui_ms = 0;
    draw_ui();
    trigger_click();

    next_due = ag_micros() + (ag_time_t)CHUNK_US;
    while (s_running) {
        ag_time_t loop0 = ag_micros();

        while (ag_poll_event(&ev, 0)) {
            if (ev.type == AG_EV_QUIT) {
                s_running = 0;
                break;
            }
            if (ev.type == AG_EV_KEY_DOWN) {
                handle_key((int)ev.key.keycode, ev.key.unicode);
            }
        }
        if (!s_running) {
            break;
        }

        {
            ag_time_t t0 = ag_micros();
            fill_mono();
            ag_ir_process_block(&s_ir, s_mono, s_stereo);
            s_render_us = (uint32_t)(ag_micros() - t0);
            if (CHUNK_US > 0u) {
                s_load_pct = (s_render_us * 100u + CHUNK_US / 2u) / CHUNK_US;
                if (s_load_pct == 0u && s_render_us > 0u) {
                    s_load_pct = 1u;
                }
            }
        }

        {
            ag_time_t t1 = ag_micros();
            if (s_audio_fd >= 0) {
                (void)ag_dev_write(s_audio_fd, s_stereo, sizeof(s_stereo));
            }
            s_send_us = (uint32_t)(ag_micros() - t1);
        }

        {
            ag_time_t now = ag_micros();
            uint32_t ms = (uint32_t)(now / 1000u);
            int want_ui = s_dirty || (ms - s_ui_ms) >= UI_PERIOD_MS;
            if (want_ui && now < next_due &&
                (uint32_t)(next_due - now) >= 2000u) {
                ag_meminfo(&s_mem);
                s_pcm_stats_ok = 0;
                if (s_audio_fd >= 0) {
                    ag_err_t st = ag_dev_ioctl(s_audio_fd, AG_IOC_AUDIO_GETSTATS,
                                               &s_pcm_stats, sizeof(s_pcm_stats));
                    s_pcm_stats_ok = (st == AG_OK) ? 1 : 0;
                }
                s_dirty = 1;
                draw_ui();
                s_ui_ms = ms;
            }
        }

        s_loop_us = (uint32_t)(ag_micros() - loop0);

        {
            ag_time_t now = ag_micros();
            if (now <= next_due) {
                uint32_t wait = (uint32_t)(next_due - now);
                if (wait > 50u) {
                    ag_delay_us(wait);
                }
                next_due += (ag_time_t)CHUNK_US;
            } else {
                s_late++;
                if (now > next_due + (ag_time_t)CHUNK_US * 4) {
                    next_due = now;
                }
                next_due += (ag_time_t)CHUNK_US;
            }
        }
    }

    if (s_audio_fd >= 0) {
        (void)ag_dev_close(s_audio_fd);
    }
    ag_wav_free(&s_dry);
    ag_ir_free(&s_ir);
    ag_cursor(true);
    ag_cls();
    return 0;
}
