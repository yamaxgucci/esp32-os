/*
 * GRAIN — graphical granular synthesizer for ArgonOS.
 *
 * Soft RGB565 UI (waveform + live grains + knobs), WAV load from HostFS,
 * pcmvirt audio, midivirt notes, mousevirt pointer.  Mic Rec is stubbed
 * until /dev/pcmin exists; Freeze + buffer append API are ready.
 *
 *   python tools/mkaxe.py --arch xtensa --gcc xtensa-esp32s3-elf-gcc ^
 *     --include sdk/include --include apps/common --include apps/common/grain ^
 *     --include apps/common/wav --include apps/common/fx ^
 *     -o build/apps/GRAIN.AXE ^
 *     apps/grain/grain.c apps/common/grain/ag_grain.c apps/common/wav/ag_wav.c ^
 *     apps/common/fx/ag_fx.c
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>

#include <argon/argon.h>
#include <argon/keys.h>

#include "audio_out.h"
#include "ag_grain.h"
#include "ag_wav.h"
#include "ag_fx.h"

AG_APP_SIZED("GRAIN", "0.1", "argon", AG_AXE_NEEDS_GFX, 12 * 1024, 768 * 1024);

#define RATE     22050u
#define CHUNK    441
#define CHUNK_US ((uint32_t)((CHUNK * 1000000ull) / RATE))
#define UI_SLACK_US 5000u
#define UI_PERIOD_MS 50u

/* Palette — charcoal / amber / cyan instrument look */
#define COL_BG     0x0012151Au
#define COL_PANEL  0x001C222Bu
#define COL_EDGE   0x002A3340u
#define COL_TEXT   0x00D6DCE4u
#define COL_MUTED  0x006A7380u
#define COL_ACCENT 0x00E8A54Bu
#define COL_GRAIN  0x005EC8D8u
#define COL_WAVE   0x006A7380u
#define COL_POS    0x00E8A54Bu
#define COL_BTN    0x0026323Du
#define COL_BTN_ON 0x003D4A38u
#define COL_DANGER 0x00C45C5Cu
#define COL_KEY_W  0x00E8ECF0u
#define COL_KEY_B  0x001A1D22u
#define COL_KEY_ON 0x00E8A54Bu

enum {
    KNOB_POS = 0,
    KNOB_SIZE,
    KNOB_DENS,
    KNOB_SPRAY,
    KNOB_PITCH,
    KNOB_TEX,
    KNOB_N
};

static const char *const k_knob_name[KNOB_N] = {
    "POS", "SIZE", "DENS", "SPRAY", "PITCH", "TEX",
};

static ag_grain_t s_g;
static ag_fx_t    s_fx;
static int        s_fx_ready;
static int16_t    s_pcm[CHUNK * 2];
static char       s_audio_path[AG_PATH_MAX];
static ag_handle_t s_audio_fd = -1;
static ag_handle_t s_midi_fd = -1;
static int        s_midi_want = 1;

static ag_gfxinfo_t s_gi;
static int          s_have_gfx;
static uint16_t     s_fb_w = 640, s_fb_h = 400;

static char     s_sample_name[48] = "(none)";
static char     s_status[80] = "";
static int      s_dirty = 1;
static int      s_picker;
static int      s_freeze;
static uint8_t  s_note_held[128];
static int      s_held_n;

static ag_time_t s_next_due;
static uint32_t  s_ui_ms;
static uint32_t  s_render_us;
static uint32_t  s_load_pct;
static uint32_t  s_late;

/* Pointer UI state */
static int s_mx = -1, s_my = -1;
static int s_mbtn;
static int s_drag_knob = -1;
static int s_drag_wave;

/* Waveform peak cache */
enum { WAVE_PTS = 320 };
static int16_t s_wave_pk[WAVE_PTS];
static int     s_wave_ready;

/* File picker */
enum { PICK_MAX = 32, PICK_PATH = 96 };
static char s_pick[PICK_MAX][PICK_PATH];
static int  s_pick_n;
static int  s_pick_i;

/* Layout */
static int16_t s_wave_x, s_wave_y;
static uint16_t s_wave_w, s_wave_h;
static int16_t s_knob_y;
static int16_t s_piano_y;

static int clampi(int v, int lo, int hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
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

static void rebuild_wave(void)
{
    uint32_t i, frames;
    const int16_t *d;
    s_wave_ready = 0;
    if (s_g.buf.data == NULL || s_g.buf.frames < 2u) {
        return;
    }
    d = s_g.buf.data;
    frames = s_g.buf.frames;
    for (i = 0; i < WAVE_PTS; i++) {
        uint32_t a = (i * frames) / WAVE_PTS;
        uint32_t b = ((i + 1u) * frames) / WAVE_PTS;
        int32_t peak = 0;
        uint32_t j;
        if (b <= a) {
            b = a + 1u;
        }
        for (j = a; j < b && j < frames; j++) {
            int32_t v = d[j];
            if (v < 0) {
                v = -v;
            }
            if (v > peak) {
                peak = v;
            }
        }
        s_wave_pk[i] = (int16_t)peak;
    }
    s_wave_ready = 1;
}

static int load_wav_path(const char *path)
{
    ag_wav_pcm_t w;
    const char *slash;
    size_t i = 0;
    if (ag_wav_load(path, &w) != 0) {
        set_status("WAV load failed");
        return -1;
    }
    if (s_g.buf.data != NULL && s_g.buf.owned) {
        ag_free(s_g.buf.data);
    }
    ag_grain_buf_set(&s_g, w.data, w.frames, w.rate, 1);
    slash = path;
    for (i = 0; path[i]; i++) {
        if (path[i] == '\\' || path[i] == '/') {
            slash = path + i + 1;
        }
    }
    for (i = 0; slash[i] && i + 1u < sizeof(s_sample_name); i++) {
        s_sample_name[i] = slash[i];
    }
    s_sample_name[i] = '\0';
    rebuild_wave();
    set_status("loaded");
    s_dirty = 1;
    return 0;
}

static void load_builtin(void)
{
    /* ~1s sine pad @ 22050 if no WAV */
    enum { N = 22050 };
    int16_t *buf = (int16_t *)ag_malloc(N * sizeof(int16_t));
    uint32_t i;
    if (buf == NULL) {
        set_status("OOM builtin");
        return;
    }
    for (i = 0; i < N; i++) {
        /* crude 220 Hz + 330 Hz */
        int32_t p1 = (int32_t)((i * 220u * 65536u) / RATE);
        int32_t p2 = (int32_t)((i * 330u * 65536u) / RATE);
        int32_t s = ((p1 & 0x7fff) - 16384) + (((p2 & 0x7fff) - 16384) / 2);
        uint32_t env = i < 2000u ? i : (i > N - 4000u ? (N - i) : 2000u);
        s = (s * (int32_t)env) / 2000;
        if (s > 32767) {
            s = 32767;
        }
        if (s < -32768) {
            s = -32768;
        }
        buf[i] = (int16_t)s;
    }
    if (s_g.buf.data != NULL && s_g.buf.owned) {
        ag_free(s_g.buf.data);
    }
    ag_grain_buf_set(&s_g, buf, N, RATE, 1);
    memcpy(s_sample_name, "builtin", 8);
    rebuild_wave();
    set_status("builtin sample");
}

static int ends_with_wav(const char *name)
{
    size_t n = 0;
    while (name[n]) {
        n++;
    }
    if (n < 4u) {
        return 0;
    }
    {
        char c0 = name[n - 4], c1 = name[n - 3], c2 = name[n - 2],
             c3 = name[n - 1];
        if (c0 == '.') {
            if (c1 >= 'A' && c1 <= 'Z') {
                c1 = (char)(c1 - 'A' + 'a');
            }
            if (c2 >= 'A' && c2 <= 'Z') {
                c2 = (char)(c2 - 'A' + 'a');
            }
            if (c3 >= 'A' && c3 <= 'Z') {
                c3 = (char)(c3 - 'A' + 'a');
            }
            return c1 == 'w' && c2 == 'a' && c3 == 'v';
        }
    }
    return 0;
}

static void scan_wav_dir(const char *dir)
{
    ag_handle_t d;
    ag_dirent_t ent;
    d = ag_opendir(dir);
    if (d < 0) {
        return;
    }
    while (ag_readdir(d, &ent) == AG_OK && s_pick_n < PICK_MAX) {
        char path[PICK_PATH];
        size_t i = 0, j = 0;
        if (ent.name[0] == '.') {
            continue;
        }
        if (!ends_with_wav(ent.name)) {
            continue;
        }
        while (dir[i] && i + 1u < sizeof(path)) {
            path[i] = dir[i];
            i++;
        }
        if (i > 0 && path[i - 1] != '\\' && path[i - 1] != '/' &&
            i + 1u < sizeof(path)) {
            path[i++] = '\\';
        }
        while (ent.name[j] && i + 1u < sizeof(path)) {
            path[i++] = ent.name[j++];
        }
        path[i] = '\0';
        memcpy(s_pick[s_pick_n], path, sizeof(path));
        s_pick_n++;
    }
    (void)ag_closedir(d);
}

static void open_picker(void)
{
    s_pick_n = 0;
    s_pick_i = 0;
    scan_wav_dir("h:\\grain");
    scan_wav_dir("h:");
    scan_wav_dir("t:\\grain");
    if (s_pick_n == 0) {
        set_status("no WAV on H:\\grain");
        s_picker = 0;
        return;
    }
    s_picker = 1;
    s_dirty = 1;
}

static int open_sink(void)
{
    s_audio_fd = ag_audio_out_open_dev(s_audio_path, RATE, 2);
    if (s_audio_fd < 0) {
        (void)ag_audio_out_resolve("pcmnull", s_audio_path, sizeof(s_audio_path));
        s_audio_fd = ag_audio_out_open_dev(s_audio_path, RATE, 2);
    }
    return s_audio_fd < 0 ? -1 : 0;
}

static void open_midivirt(void)
{
    if (!s_midi_want) {
        return;
    }
    s_midi_fd = ag_dev_open("/dev/midivirt");
    if (s_midi_fd >= 0) {
        ag_printf("grain: MIDI-in = /dev/midivirt\n");
    }
}

static void pump_midivirt(void)
{
    uint8_t buf[64];
    int32_t n;
    if (s_midi_fd < 0) {
        return;
    }
    n = ag_dev_read(s_midi_fd, buf, sizeof(buf));
    if (n <= 0) {
        return;
    }
    {
        int32_t i;
        for (i = 0; i + 3 < n; i += 4) {
            uint8_t st = buf[i];
            uint8_t d1 = buf[i + 1];
            uint8_t d2 = buf[i + 2];
            uint8_t hi = (uint8_t)(st & 0xf0u);
            if (hi == 0x90u && d2 > 0u) {
                if (!s_note_held[d1 & 127u]) {
                    s_held_n++;
                }
                s_note_held[d1 & 127u] = 1;
                ag_grain_note_on(&s_g, d1, d2);
                s_dirty = 1;
            } else if (hi == 0x80u || (hi == 0x90u && d2 == 0u)) {
                if (s_note_held[d1 & 127u]) {
                    s_held_n--;
                    s_note_held[d1 & 127u] = 0;
                }
                ag_grain_note_off(&s_g, d1);
                s_dirty = 1;
            } else if (hi == 0xb0u && d1 == 123u) {
                ag_grain_all_notes_off(&s_g);
                memset(s_note_held, 0, sizeof(s_note_held));
                s_held_n = 0;
                s_dirty = 1;
            }
        }
    }
}

static void note_set(uint8_t note, int on)
{
    note &= 127u;
    if (on) {
        if (!s_note_held[note]) {
            s_held_n++;
        }
        s_note_held[note] = 1;
        ag_grain_note_on(&s_g, note, 100);
    } else {
        if (s_note_held[note]) {
            s_held_n--;
            s_note_held[note] = 0;
        }
        ag_grain_note_off(&s_g, note);
    }
    s_dirty = 1;
}

/* Piano map matches DX7 / midikbd */
static int piano_note(int key, uint32_t uni)
{
    (void)uni;
    switch (key) {
    case AG_KEY_Z: return 60;
    case AG_KEY_S: return 61;
    case AG_KEY_X: return 62;
    case AG_KEY_D: return 63;
    case AG_KEY_C: return 64;
    case AG_KEY_V: return 65;
    case AG_KEY_G: return 66;
    case AG_KEY_B: return 67;
    case AG_KEY_H: return 68;
    case AG_KEY_N: return 69;
    case AG_KEY_J: return 70;
    case AG_KEY_M: return 71;
    case AG_KEY_Q: return 72;
    case AG_KEY_2: return 73;
    case AG_KEY_W: return 74;
    case AG_KEY_3: return 75;
    case AG_KEY_E: return 76;
    case AG_KEY_R: return 77;
    case AG_KEY_5: return 78;
    case AG_KEY_T: return 79;
    case AG_KEY_6: return 80;
    case AG_KEY_Y: return 81;
    case AG_KEY_7: return 82;
    case AG_KEY_U: return 83;
    case AG_KEY_I: return 84;
    default: return -1;
    }
}

static uint8_t *param_ptr(int knob)
{
    switch (knob) {
    case KNOB_POS: return &s_g.params.position;
    case KNOB_SIZE: return &s_g.params.size;
    case KNOB_DENS: return &s_g.params.density;
    case KNOB_SPRAY: return &s_g.params.spray;
    case KNOB_TEX: return &s_g.params.texture;
    default: return NULL;
    }
}

static void knob_adjust(int knob, int delta)
{
    if (knob == KNOB_PITCH) {
        s_g.params.pitch =
            (int8_t)clampi((int)s_g.params.pitch + delta, -48, 48);
    } else {
        uint8_t *p = param_ptr(knob);
        if (p) {
            *p = (uint8_t)clampi((int)*p + delta, 0, 127);
        }
    }
    s_dirty = 1;
}

static void knob_set_norm(int knob, int y, int y0, int h)
{
    /* top = 127, bottom = 0 */
    int v = 127 - ((y - y0) * 127) / (h > 1 ? h - 1 : 1);
    v = clampi(v, 0, 127);
    if (knob == KNOB_PITCH) {
        s_g.params.pitch = (int8_t)(v - 64);
    } else {
        uint8_t *p = param_ptr(knob);
        if (p) {
            *p = (uint8_t)v;
        }
    }
    s_dirty = 1;
}

static int knob_value(int knob)
{
    if (knob == KNOB_PITCH) {
        return (int)s_g.params.pitch + 64;
    }
    {
        uint8_t *p = param_ptr(knob);
        return p ? (int)*p : 0;
    }
}

static void layout_geom(void)
{
    s_wave_x = 16;
    s_wave_y = 36;
    s_wave_w = (uint16_t)(s_fb_w - 32);
    s_wave_h = 150;
    s_knob_y = (int16_t)(s_wave_y + (int)s_wave_h + 16);
    s_piano_y = (int16_t)(s_fb_h - 56);
}

static void draw_text(int16_t x, int16_t y, const char *s, uint32_t fg)
{
    (void)ag_gfx_text(x, y, s, fg, COL_BG);
}

static void draw_knob(int idx, int16_t x, int16_t y)
{
    const int kw = 56, kh = 72;
    int v = knob_value(idx);
    int fill = (v * (kh - 20)) / 127;
    char num[8];
    ag_gfx_fill_round_rect(x, y, kw, kh, 6, COL_PANEL);
    ag_gfx_stroke_rect(x, y, kw, kh, COL_EDGE);
    ag_gfx_fill_rect((int16_t)(x + 18), (int16_t)(y + 14), 20, (uint16_t)(kh - 28),
                     COL_EDGE);
    ag_gfx_fill_rect((int16_t)(x + 18),
                     (int16_t)(y + 14 + (kh - 28) - fill), 20, (uint16_t)fill,
                     COL_ACCENT);
    draw_text((int16_t)(x + 10), (int16_t)(y + kh - 14), k_knob_name[idx],
              COL_MUTED);
    /* value */
    if (idx == KNOB_PITCH) {
        int p = (int)s_g.params.pitch;
        num[0] = (p < 0) ? '-' : '+';
        p = p < 0 ? -p : p;
        num[1] = (char)('0' + (p / 10) % 10);
        num[2] = (char)('0' + p % 10);
        num[3] = '\0';
    } else {
        num[0] = (char)('0' + (v / 100) % 10);
        num[1] = (char)('0' + (v / 10) % 10);
        num[2] = (char)('0' + v % 10);
        num[3] = '\0';
    }
    draw_text((int16_t)(x + 14), (int16_t)(y + 2), num, COL_TEXT);
    (void)kw;
}

static int hit_knob(int x, int y)
{
    int i;
    int gap = (int)s_fb_w / (KNOB_N + 1);
    for (i = 0; i < KNOB_N; i++) {
        int16_t kx = (int16_t)(gap * (i + 1) - 28);
        if (x >= kx && x < kx + 56 && y >= s_knob_y && y < s_knob_y + 72) {
            return i;
        }
    }
    return -1;
}

static void draw_waveform(void)
{
    int16_t x = s_wave_x, y = s_wave_y;
    uint16_t w = s_wave_w, h = s_wave_h;
    int mid = y + (int)h / 2;
    uint32_t i;
    int pos_x, spray_w;

    ag_gfx_fill_round_rect(x, y, w, h, 8, COL_PANEL);
    ag_gfx_stroke_rect(x, y, w, h, COL_EDGE);

    if (s_wave_ready) {
        for (i = 0; i < WAVE_PTS; i++) {
            int px = x + (int)((i * (w - 2u)) / WAVE_PTS) + 1;
            int amp = ((int)s_wave_pk[i] * ((int)h / 2 - 4)) / 32768;
            if (amp < 1) {
                amp = 1;
            }
            ag_gfx_line((int16_t)px, (int16_t)(mid - amp), (int16_t)px,
                        (int16_t)(mid + amp), COL_WAVE);
        }
    }

    /* position + spray window */
    pos_x = x + 1 + ((int)s_g.params.position * ((int)w - 2)) / 127;
    spray_w = ((int)s_g.params.spray * ((int)w - 2)) / 127;
    if (spray_w < 2) {
        spray_w = 2;
    }
    {
        int x0 = pos_x - spray_w / 2;
        if (x0 < x + 1) {
            x0 = x + 1;
        }
        ag_gfx_fill_rect((int16_t)x0, y, (uint16_t)spray_w, h, 0x00283A28u);
    }
    ag_gfx_line((int16_t)pos_x, y, (int16_t)pos_x, (int16_t)(y + (int)h - 1),
                COL_POS);

    /* live grains */
    for (i = 0; i < s_g.viz_n; i++) {
        int gx = x + 1 + ((int)s_g.viz[i].x_q15 * ((int)w - 4)) / 32767;
        int gy = y + 4 + ((255 - (int)s_g.viz[i].y) * ((int)h - 8)) / 255;
        uint32_t c = COL_GRAIN;
        if (s_g.viz[i].a < 80u) {
            c = COL_MUTED;
        }
        ag_gfx_fill_circle((int16_t)gx, (int16_t)gy, 3, c);
    }
}

static void draw_buttons(void)
{
    /* Load / Freeze / Rec */
    static const char *names[3] = { "Load", "Freeze", "Rec" };
    int i;
    for (i = 0; i < 3; i++) {
        int16_t bx = (int16_t)(s_fb_w - 16 - (3 - i) * 72);
        int16_t by = 8;
        uint32_t bg = COL_BTN;
        if (i == 1 && s_freeze) {
            bg = COL_BTN_ON;
        }
        if (i == 2) {
            bg = 0x00302828u; /* stub */
        }
        ag_gfx_fill_round_rect(bx, by, 64, 22, 4, bg);
        ag_gfx_stroke_rect(bx, by, 64, 22, COL_EDGE);
        draw_text((int16_t)(bx + 12), (int16_t)(by + 4), names[i],
                  i == 2 ? COL_MUTED : COL_TEXT);
    }
}

static int hit_button(int x, int y)
{
    int i;
    if (y < 8 || y > 30) {
        return -1;
    }
    for (i = 0; i < 3; i++) {
        int16_t bx = (int16_t)(s_fb_w - 16 - (3 - i) * 72);
        if (x >= bx && x < bx + 64) {
            return i;
        }
    }
    return -1;
}

static void u8_to_dec(char *o, unsigned v)
{
    o[0] = (char)('0' + (v / 100u) % 10u);
    o[1] = (char)('0' + (v / 10u) % 10u);
    o[2] = (char)('0' + v % 10u);
    o[3] = '\0';
}

static void draw_adsr_fx(void)
{
    char line[72];
    char a[4], d[4], s[4], r[4], w[4], g[4], l[4];
    int16_t x = 16;
    int16_t y = (int16_t)(s_knob_y + 80);
    ag_gfx_fill_round_rect(x, y, (uint16_t)(s_fb_w - 32), 36, 6, COL_PANEL);
    u8_to_dec(a, s_g.params.attack);
    u8_to_dec(d, s_g.params.decay);
    u8_to_dec(s, s_g.params.sustain);
    u8_to_dec(r, s_g.params.release);
    u8_to_dec(w, s_fx_ready ? s_fx.master_wet : 0u);
    u8_to_dec(g, s_g.active_grains);
    u8_to_dec(l, s_load_pct);
    /* "ADSR aaa/ddd/sss/rrr  FX www  ggg gr  load lll%" */
    {
        int i = 0;
        const char *p;
        const char *parts[] = { "ADSR ", a, "/", d, "/", s, "/", r,
                                "  FX ", w, "  ", g, " gr  load ", l, "%",
                                NULL };
        int pi;
        for (pi = 0; parts[pi]; pi++) {
            p = parts[pi];
            while (*p && i + 1 < (int)sizeof(line)) {
                line[i++] = *p++;
            }
        }
        line[i] = '\0';
    }
    draw_text((int16_t)(x + 8), (int16_t)(y + 10), line, COL_MUTED);
}

static void draw_piano(void)
{
    int i;
    int nkeys = 25; /* C4..C6 */
    int kw = ((int)s_fb_w - 32) / nkeys;
    int16_t y = s_piano_y;
    ag_gfx_fill_rect(0, y, s_fb_w, (uint16_t)(s_fb_h - y), COL_BG);
    for (i = 0; i < nkeys; i++) {
        int note = 60 + i;
        int pc = note % 12;
        int black = (pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10);
        int16_t x = (int16_t)(16 + i * kw);
        uint32_t c = black ? COL_KEY_B : COL_KEY_W;
        if (s_note_held[note]) {
            c = COL_KEY_ON;
        }
        ag_gfx_fill_rect(x, y, (uint16_t)(kw - 1), 40, c);
        if (!black) {
            ag_gfx_stroke_rect(x, y, (uint16_t)(kw - 1), 40, COL_EDGE);
        }
    }
}

static void draw_picker(void)
{
    int i;
    int16_t x = 80, y = 60;
    uint16_t w = (uint16_t)(s_fb_w - 160), h = 240;
    ag_gfx_fill_round_rect(x, y, w, h, 8, COL_PANEL);
    ag_gfx_stroke_rect(x, y, w, h, COL_ACCENT);
    draw_text((int16_t)(x + 12), (int16_t)(y + 8), "Load WAV  (Enter/click, Esc)",
              COL_ACCENT);
    for (i = 0; i < s_pick_n && i < 12; i++) {
        uint32_t fg = (i == s_pick_i) ? COL_ACCENT : COL_TEXT;
        if (i == s_pick_i) {
            ag_gfx_fill_rect((int16_t)(x + 8), (int16_t)(y + 28 + i * 16),
                             (uint16_t)(w - 16), 16, COL_BTN);
        }
        draw_text((int16_t)(x + 12), (int16_t)(y + 28 + i * 16), s_pick[i], fg);
    }
}

static void draw_ui(void)
{
    int i;
    int gap;
    char top[96];

    if (!s_have_gfx) {
        return;
    }
    ag_gfx_clip_reset();
    ag_gfx_clear(COL_BG);

    {
        int ti = 0;
        const char *pref = "GRAIN   ";
        const char *nm = s_sample_name;
        while (*pref && ti + 1 < (int)sizeof(top)) {
            top[ti++] = *pref++;
        }
        while (*nm && ti + 1 < (int)sizeof(top)) {
            top[ti++] = *nm++;
        }
        top[ti] = '\0';
    }
    draw_text(16, 10, top, COL_ACCENT);
    if (s_status[0]) {
        draw_text(200, 10, s_status, COL_MUTED);
    }
    draw_buttons();
    draw_waveform();

    gap = (int)s_fb_w / (KNOB_N + 1);
    for (i = 0; i < KNOB_N; i++) {
        int16_t kx = (int16_t)(gap * (i + 1) - 28);
        draw_knob(i, kx, s_knob_y);
    }
    draw_adsr_fx();
    draw_piano();

    /* cursor */
    if (s_mx >= 0) {
        ag_gfx_line((int16_t)s_mx, (int16_t)(s_my - 4), (int16_t)s_mx,
                    (int16_t)(s_my + 4), COL_ACCENT);
        ag_gfx_line((int16_t)(s_mx - 4), (int16_t)s_my, (int16_t)(s_mx + 4),
                    (int16_t)s_my, COL_ACCENT);
    }

    if (s_picker) {
        draw_picker();
    }

    ag_gfx_flush(0, 0, 0, 0);
}

static void do_button(int id)
{
    if (id == 0) {
        open_picker();
    } else if (id == 1) {
        s_freeze = !s_freeze;
        ag_grain_freeze(&s_g, s_freeze);
        set_status(s_freeze ? "freeze on" : "freeze off");
    } else if (id == 2) {
        set_status("Rec: no capture device");
    }
}

static void pointer_event(const ag_event_t *ev)
{
    int x = ev->ptr.x;
    int y = ev->ptr.y;
    s_mx = x;
    s_my = y;

    if (ev->type == AG_EV_POINTER_DOWN) {
        s_mbtn = 1;
        if (s_picker) {
            int row = (y - 88) / 16;
            if (row >= 0 && row < s_pick_n) {
                s_pick_i = row;
                (void)load_wav_path(s_pick[s_pick_i]);
                s_picker = 0;
            }
            s_dirty = 1;
            return;
        }
        {
            int b = hit_button(x, y);
            if (b >= 0) {
                do_button(b);
                return;
            }
        }
        {
            int k = hit_knob(x, y);
            if (k >= 0) {
                s_drag_knob = k;
                knob_set_norm(k, y, s_knob_y + 14, 44);
                return;
            }
        }
        if (x >= s_wave_x && x < s_wave_x + (int)s_wave_w && y >= s_wave_y &&
            y < s_wave_y + (int)s_wave_h) {
            int rel = x - s_wave_x;
            s_drag_wave = 1;
            s_g.params.position =
                (uint8_t)clampi((rel * 127) / (int)s_wave_w, 0, 127);
            s_dirty = 1;
            return;
        }
        if (y >= s_piano_y) {
            int nkeys = 25;
            int kw = ((int)s_fb_w - 32) / nkeys;
            int idx = (x - 16) / kw;
            if (idx >= 0 && idx < nkeys) {
                note_set((uint8_t)(60 + idx), 1);
            }
        }
    } else if (ev->type == AG_EV_POINTER_UP) {
        if (s_mbtn && y >= s_piano_y) {
            int nkeys = 25;
            int kw = ((int)s_fb_w - 32) / nkeys;
            int idx = (x - 16) / kw;
            if (idx >= 0 && idx < nkeys) {
                note_set((uint8_t)(60 + idx), 0);
            }
        }
        s_mbtn = 0;
        s_drag_knob = -1;
        s_drag_wave = 0;
    } else if (ev->type == AG_EV_POINTER_MOVE) {
        if (s_drag_knob >= 0) {
            knob_set_norm(s_drag_knob, y, s_knob_y + 14, 44);
        } else if (s_drag_wave) {
            int rel = x - s_wave_x;
            s_g.params.position =
                (uint8_t)clampi((rel * 127) / (int)s_wave_w, 0, 127);
            s_dirty = 1;
        }
        s_dirty = 1;
    } else if (ev->type == AG_EV_WHEEL) {
        int k = hit_knob(x, y);
        if (k >= 0) {
            knob_adjust(k, ev->ptr.dy > 0 ? -3 : 3);
        } else if (x >= s_wave_x && x < s_wave_x + (int)s_wave_w) {
            s_g.params.spray = (uint8_t)clampi(
                (int)s_g.params.spray + (ev->ptr.dy > 0 ? -4 : 4), 0, 127);
            s_dirty = 1;
        }
    }
}

static void handle_key(int key, int down, uint32_t uni)
{
    int note;
    if (s_picker) {
        if (!down) {
            return;
        }
        if (key == AG_KEY_ESC) {
            s_picker = 0;
            s_dirty = 1;
            return;
        }
        if (key == AG_KEY_UP) {
            if (s_pick_i > 0) {
                s_pick_i--;
            }
            s_dirty = 1;
            return;
        }
        if (key == AG_KEY_DOWN) {
            if (s_pick_i + 1 < s_pick_n) {
                s_pick_i++;
            }
            s_dirty = 1;
            return;
        }
        if (key == AG_KEY_ENTER || key == AG_KEY_SPACE) {
            (void)load_wav_path(s_pick[s_pick_i]);
            s_picker = 0;
            return;
        }
        return;
    }

    note = piano_note(key, uni);
    if (note >= 0) {
        note_set((uint8_t)note, down);
        return;
    }
    if (!down) {
        return;
    }
    if (key == AG_KEY_ESC) {
        ag_event_t q;
        memset(&q, 0, sizeof(q));
        q.type = AG_EV_QUIT;
        (void)ag_inject_event(&q);
        return;
    }
    if (key == AG_KEY_L) {
        open_picker();
    } else if (key == AG_KEY_F) {
        do_button(1);
    } else if (key == AG_KEY_LEFT) {
        knob_adjust(KNOB_POS, -2);
    } else if (key == AG_KEY_RIGHT) {
        knob_adjust(KNOB_POS, 2);
    } else if (key == AG_KEY_UP) {
        knob_adjust(KNOB_DENS, 2);
    } else if (key == AG_KEY_DOWN) {
        knob_adjust(KNOB_DENS, -2);
    } else if (key == AG_KEY_MINUS || key == AG_KEY_KP_MINUS) {
        knob_adjust(KNOB_PITCH, -1);
    } else if (key == AG_KEY_EQUAL || key == AG_KEY_KP_PLUS) {
        knob_adjust(KNOB_PITCH, 1);
    } else if (key == AG_KEY_LBRACKET) {
        knob_adjust(KNOB_SIZE, -2);
    } else if (key == AG_KEY_RBRACKET) {
        knob_adjust(KNOB_SIZE, 2);
    } else if (key == AG_KEY_COMMA) {
        s_g.params.attack = (uint8_t)clampi((int)s_g.params.attack - 4, 0, 127);
        s_dirty = 1;
    } else if (key == AG_KEY_PERIOD) {
        s_g.params.attack = (uint8_t)clampi((int)s_g.params.attack + 4, 0, 127);
        s_dirty = 1;
    } else if (key == AG_KEY_SEMICOLON) {
        if (s_fx_ready) {
            s_fx.master_wet =
                (uint8_t)clampi((int)s_fx.master_wet - 8, 0, 127);
            s_dirty = 1;
        }
    } else if (key == AG_KEY_APOSTROPHE) {
        if (s_fx_ready) {
            s_fx.master_wet =
                (uint8_t)clampi((int)s_fx.master_wet + 8, 0, 127);
            s_dirty = 1;
        }
    } else if (key == AG_KEY_SPACE) {
        ag_grain_all_notes_off(&s_g);
        memset(s_note_held, 0, sizeof(s_note_held));
        s_held_n = 0;
        s_dirty = 1;
    }
}

static void pace_wait(ag_time_t due)
{
    for (;;) {
        ag_time_t now = ag_micros();
        if (now >= due) {
            return;
        }
        {
            uint32_t rem = (uint32_t)(due - now);
            if (rem > 1000u) {
                ag_delay((rem / 1000u) - 1u);
            }
        }
    }
}

static void pump_audio(void)
{
    ag_time_t t0 = ag_micros();
    int32_t n;
    ag_grain_render(&s_g, s_pcm, CHUNK);
    if (s_fx_ready && s_fx.master_wet > 0u) {
        ag_fx_process(&s_fx, s_pcm, (int32_t)CHUNK);
    }
    ag_grain_viz_update(&s_g);
    s_render_us = (uint32_t)(ag_micros() - t0);
    s_load_pct = CHUNK_US ? (s_render_us * 100u + CHUNK_US / 2u) / CHUNK_US : 0u;

    if (s_audio_fd < 0) {
        return;
    }
    n = ag_dev_write(s_audio_fd, s_pcm, sizeof(s_pcm));
    if (n < (int32_t)sizeof(s_pcm)) {
        s_late++;
    }
}

static int parse_arg(const char *a)
{
    if (ag_audio_out_resolve(a, s_audio_path, sizeof(s_audio_path))) {
        return 0;
    }
    if (a[0] && (a[1] == ':' || a[0] == '/' || a[0] == 'h' || a[0] == 'H' ||
                 a[0] == 't' || a[0] == 'T')) {
        size_t n = strlen(a);
        if (n > 4 && (a[n - 1] == 'v' || a[n - 1] == 'V')) {
            (void)load_wav_path(a);
            return 0;
        }
    }
    if (strcmp(a, "nomidi") == 0) {
        s_midi_want = 0;
        return 0;
    }
    return -1;
}

int ag_main(int argc, char **argv)
{
    ag_event_t ev;
    int i;

    (void)ag_audio_out_resolve("pcmvirt", s_audio_path, sizeof(s_audio_path));
    for (i = 1; i < argc; i++) {
        if (parse_arg(argv[i]) != 0) {
            ag_printf("grain: arg '%s' (pcmvirt|pcmnull|path.wav|nomidi)\n",
                      argv[i]);
        }
    }

    if (ag_api()->gfx == NULL || ag_gfx_acquire(&s_gi) != AG_OK) {
        ag_printf("grain: gfx required\n");
        return 1;
    }
    s_have_gfx = 1;
    s_fb_w = s_gi.width ? s_gi.width : 640;
    s_fb_h = s_gi.height ? s_gi.height : 400;
    layout_geom();

    ag_grain_init(&s_g, RATE);
    if (s_g.buf.data == NULL) {
        if (load_wav_path("h:\\grain\\demo.wav") != 0 &&
            load_wav_path("h:\\demo.wav") != 0) {
            load_builtin();
        }
    }

    if (open_sink() != 0) {
        ag_printf("grain: no audio sink\n");
        ag_gfx_release();
        return 1;
    }
    ag_printf("grain: sound = %s @ %u Hz\n", s_audio_path, (unsigned)RATE);

    if (ag_fx_init(&s_fx, RATE) == 0) {
        s_fx_ready = 1;
        ag_fx_set_defaults(&s_fx);
        ag_fx_set_enable(&s_fx, AG_FX_DELAY | AG_FX_REVERB);
        s_fx.master_wet = 40;
        s_fx.rev_wet = 50;
        s_fx.delay_mix = 20;
    }

    open_midivirt();
    ag_printf("\x1b[>3u");
    ag_printf("\x1b[?9001h");

    s_next_due = ag_micros() + (ag_time_t)CHUNK_US;
    s_ui_ms = ag_millis();
    draw_ui();

    for (;;) {
        ag_time_t loop0 = ag_micros();

        while (ag_poll_event(&ev, 0)) {
            if (ev.type == AG_EV_QUIT) {
                goto done;
            }
            if (ev.type == AG_EV_KEY_DOWN || ev.type == AG_EV_KEY_UP) {
                handle_key((int)ev.key.keycode, ev.type == AG_EV_KEY_DOWN,
                           ev.key.unicode);
            } else if (ev.type == AG_EV_POINTER_DOWN ||
                       ev.type == AG_EV_POINTER_UP ||
                       ev.type == AG_EV_POINTER_MOVE ||
                       ev.type == AG_EV_WHEEL) {
                pointer_event(&ev);
            }
        }
        if (ag_interrupted()) {
            break;
        }

        pump_midivirt();
        pump_audio();

        {
            ag_time_t now = ag_micros();
            uint32_t ms = ag_millis();
            int want_ui = s_dirty || (ms - s_ui_ms) >= UI_PERIOD_MS;
            if (want_ui && now < s_next_due &&
                (uint32_t)(s_next_due - now) >= UI_SLACK_US) {
                s_ui_ms = ms;
                s_dirty = 0;
                draw_ui();
                now = ag_micros();
            }
            if (now <= s_next_due) {
                pace_wait(s_next_due);
                s_next_due += (ag_time_t)CHUNK_US;
            } else {
                s_late++;
                s_next_due = now + (ag_time_t)CHUNK_US;
            }
        }
        (void)loop0;
        ag_heartbeat();
    }

done:
    ag_grain_all_notes_off(&s_g);
    if (s_midi_fd >= 0) {
        (void)ag_dev_close(s_midi_fd);
    }
    if (s_audio_fd >= 0) {
        (void)ag_dev_close(s_audio_fd);
    }
    if (s_fx_ready) {
        ag_fx_free(&s_fx);
    }
    if (s_g.buf.data != NULL && s_g.buf.owned) {
        ag_free(s_g.buf.data);
        s_g.buf.data = NULL;
    }
    ag_printf("\x1b[?9001l");
    ag_printf("\x1b[<u");
    if (s_have_gfx) {
        ag_gfx_release();
    }
    return 0;
}
