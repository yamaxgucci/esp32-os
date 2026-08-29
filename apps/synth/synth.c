/*
 * SYNTH — VA / N-op FM demo on ag_synth + ag_pcm.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>

#include <argon/argon.h>
#include <argon/keys.h>

#include "ag_fx.h"
#include "ag_ir.h"
#include "ag_pcm.h"
#include "ag_synth.h"
#include "audio_out.h"

AG_APP_SIZED("SYNTH", "0.1", "argon", AG_AXE_NEEDS_AUDIO, 10 * 1024, 512 * 1024);

#define RATE  22050u
#define CHUNK AG_IR_BLOCK

static ag_synth_t s_s;
static ag_pcm_t   s_out;
static ag_fx_t    s_fx;
static ag_ir_t    s_ir;
static ag_handle_t s_midi_fd = -1;
static int        s_fx_ok;
static int        s_ir_ok;
static int        s_fx_on;
static int        s_ir_on;
static int        s_cab = 3;
static int16_t    s_pcm[CHUNK * 2];
static int16_t    s_wt[1024];
static int16_t    s_ir_mono[AG_IR_BLOCK];
static int16_t    s_ir_st[AG_IR_BLOCK * 2];
static uint8_t    s_held[128];
static int        s_running = 1;
static uint32_t   s_wav_until;
static int        s_dirty = 1;
static uint32_t   s_ui_ms;

static const uint8_t k_keys[] = {
    'z', 's', 'x', 'd', 'c', 'v', 'g', 'b', 'h', 'n', 'j', 'm',
    'q', '2', 'w', '3', 'e', 'r', '5', 't', '6', 'y', '7', 'u', 'i'
};
static const uint8_t k_notes[] = {
    60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71,
    72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83, 84
};

static int piano_note(int key)
{
    unsigned i;
    if (key >= 'A' && key <= 'Z') {
        key = key - 'A' + 'a';
    }
    for (i = 0; i < sizeof(k_keys); i++) {
        if (k_keys[i] == (uint8_t)key) {
            return (int)k_notes[i];
        }
    }
    return -1;
}

static void apply_ir(int16_t *stereo, int32_t frames)
{
    int32_t i;
    if (!s_ir_ok || !s_ir_on || frames != (int32_t)AG_IR_BLOCK) {
        return;
    }
    for (i = 0; i < frames; i++) {
        int32_t m = ((int32_t)stereo[i * 2] + (int32_t)stereo[i * 2 + 1]) >> 1;
        s_ir_mono[i] = (int16_t)m;
    }
    ag_ir_process_block(&s_ir, s_ir_mono, s_ir_st);
    for (i = 0; i < frames; i++) {
        stereo[i * 2] = s_ir_st[i * 2];
        stereo[i * 2 + 1] = s_ir_st[i * 2 + 1];
    }
}

static void open_midivirt(void)
{
    s_midi_fd = ag_dev_open("/dev/midivirt");
    if (s_midi_fd >= 0) {
        ag_printf("synth: MIDI-in = /dev/midivirt\n");
    }
}

static void pump_midivirt(void)
{
    uint8_t buf[64];
    int32_t n, i;
    if (s_midi_fd < 0) {
        return;
    }
    n = ag_dev_read(s_midi_fd, buf, sizeof(buf));
    if (n <= 0) {
        return;
    }
    for (i = 0; i + 3 < n; i += 4) {
        uint8_t st = buf[i];
        uint8_t d1 = buf[i + 1];
        uint8_t d2 = buf[i + 2];
        uint8_t hi = (uint8_t)(st & 0xf0u);
        if (hi == 0x90u && d2 > 0u) {
            s_held[d1 & 127u] = 1;
            ag_synth_note_on(&s_s, d1, d2);
        } else if (hi == 0x80u || (hi == 0x90u && d2 == 0u)) {
            s_held[d1 & 127u] = 0;
            ag_synth_note_off(&s_s, d1);
        } else if (hi == 0xb0u && d1 == 123u) {
            ag_synth_all_notes_off(&s_s);
            memset(s_held, 0, sizeof(s_held));
        }
    }
}

static void draw_ui(void)
{
    const char *eng =
        ag_synth_get(&s_s, AG_SYNTH_P_ENGINE) ? "FM" : "VA";
    const char *dist =
        ag_synth_get(&s_s, AG_SYNTH_P_DIST_MODEL) == AG_DIST_TUBE ? "tube"
                                                                  : "jfet";
    ag_cls();
    ag_printf("SYNTH  engine=%s  sink=%s\n", eng, s_out.path);
    ag_printf("cut %d  reso %d  mix %d  pwm %d  wave %d/%d\n",
              (int)ag_synth_get(&s_s, AG_SYNTH_P_CUTOFF),
              (int)ag_synth_get(&s_s, AG_SYNTH_P_RESO),
              (int)ag_synth_get(&s_s, AG_SYNTH_P_OSC_MIX),
              (int)ag_synth_get(&s_s, AG_SYNTH_P_PWM),
              (int)ag_synth_get(&s_s, AG_SYNTH_P_WAVE1),
              (int)ag_synth_get(&s_s, AG_SYNTH_P_WAVE2));
    ag_printf("drive %d  %s  sag %d  ops %d  idx %d\n",
              (int)ag_synth_get(&s_s, AG_SYNTH_P_DRIVE), dist,
              (int)ag_synth_get(&s_s, AG_SYNTH_P_SAG),
              (int)ag_synth_get(&s_s, AG_SYNTH_P_FM_OPS),
              (int)ag_synth_get(&s_s, AG_SYNTH_P_FM_INDEX));
    ag_printf("fx %s  cab %s (preset %d)  load %u%%  late %u\n",
              s_fx_on ? "on" : "off", s_ir_on ? "on" : "off", s_cab,
              (unsigned)s_out.load_pct, (unsigned)s_out.late);
    ag_printf("Z-M / Q-I notes  [ ] cut  - = reso  1-6 wave (6=wavetable)\n");
    ag_printf("D drive  T tube/jfet  F VA/FM  O ops  L LFO->cut\n");
    ag_printf("X fx  I cab  C cab preset  Space panic  Esc quit\n");
    s_dirty = 0;
}

static void nudge(uint16_t p, int d)
{
    ag_synth_set(&s_s, p, ag_synth_get(&s_s, p) + d);
    s_dirty = 1;
}

static void handle_key(int key, int down, int ch)
{
    int note;
    if (!down) {
        note = piano_note(ch ? ch : key);
        if (note < 0) {
            note = piano_note(key);
        }
        if (note >= 0 && s_held[note]) {
            s_held[note] = 0;
            ag_synth_note_off(&s_s, (uint8_t)note);
        }
        return;
    }
    if (key == AG_KEY_ESC) {
        s_running = 0;
        return;
    }
    if (key == ' ' || ch == ' ') {
        ag_synth_all_notes_off(&s_s);
        memset(s_held, 0, sizeof(s_held));
        return;
    }
    note = piano_note(ch ? ch : key);
    if (note < 0) {
        note = piano_note(key);
    }
    if (note >= 0) {
        s_held[note] = 1;
        ag_synth_note_on(&s_s, (uint8_t)note, 100);
        return;
    }
    if (ch == '[' || key == '[') {
        nudge(AG_SYNTH_P_CUTOFF, -4);
    } else if (ch == ']' || key == ']') {
        nudge(AG_SYNTH_P_CUTOFF, 4);
    } else if (ch == '-' || key == '-') {
        nudge(AG_SYNTH_P_RESO, -4);
    } else if (ch == '=' || key == '=') {
        nudge(AG_SYNTH_P_RESO, 4);
    } else if (ch >= '1' && ch <= '6') {
        ag_synth_set(&s_s, AG_SYNTH_P_WAVE1, ch - '1');
        s_dirty = 1;
    } else if (ch == 'd' || ch == 'D') {
        nudge(AG_SYNTH_P_DRIVE, 8);
    } else if (ch == 't' || ch == 'T') {
        int m = (int)ag_synth_get(&s_s, AG_SYNTH_P_DIST_MODEL);
        ag_synth_set(&s_s, AG_SYNTH_P_DIST_MODEL,
                     m == AG_DIST_TUBE ? AG_DIST_JFET : AG_DIST_TUBE);
        s_dirty = 1;
    } else if (ch == 'f' || ch == 'F') {
        int e = (int)ag_synth_get(&s_s, AG_SYNTH_P_ENGINE);
        ag_synth_set(&s_s, AG_SYNTH_P_ENGINE, e ? AG_SYNTH_VA : AG_SYNTH_FM);
        s_dirty = 1;
    } else if (ch == 'o' || ch == 'O') {
        int n = (int)ag_synth_get(&s_s, AG_SYNTH_P_FM_OPS) + 1;
        if (n > 8) {
            n = 2;
        }
        ag_synth_set(&s_s, AG_SYNTH_P_FM_OPS, n);
        s_dirty = 1;
    } else if (ch == 'l' || ch == 'L') {
        (void)ag_synth_mod_bind(&s_s, AG_SYNTH_SRC_LFO1, AG_SYNTH_P_CUTOFF, 80);
        s_dirty = 1;
    } else if (ch == 'x' || ch == 'X') {
        s_fx_on = !s_fx_on;
        s_dirty = 1;
    } else if (ch == 'i' || ch == 'I') {
        s_ir_on = !s_ir_on;
        s_dirty = 1;
    } else if (ch == 'c' || ch == 'C') {
        s_cab = (s_cab == 3) ? 4 : 3;
        if (s_ir_ok) {
            (void)ag_ir_load_preset(&s_ir, s_cab);
        }
        s_dirty = 1;
    }
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
                           "synthesis in real time");

    const char *sink = "pcmnull";
    ag_event_t  ev;
    int         i;

    for (i = 1; i < argc; i++) {
        char tmp[AG_PATH_MAX];
        if (ag_audio_out_resolve(argv[i], tmp, sizeof(tmp)) ||
            (argv[i][0] && argv[i][strlen(argv[i]) - 1] == 'v')) {
            sink = argv[i];
        }
    }

    ag_synth_init(&s_s, RATE);
    {
        uint32_t k;
        for (k = 0; k < 1024u; k++) {
            uint32_t ph = k * (1u << 22);
            int32_t  acc = (int32_t)ag_dsp_sin(ph) +
                          ((int32_t)ag_dsp_sin(ph * 2u) / 2) +
                          ((int32_t)ag_dsp_sin(ph * 3u) / 3) +
                          ((int32_t)ag_dsp_sin(ph * 4u) / 4);
            s_wt[k] = ag_sat16(acc);
        }
        ag_synth_set_wavetable(&s_s, s_wt, 1024u);
    }
    if (ag_pcm_open(&s_out, sink, RATE, 2) != 0) {
        ag_printf("synth: no audio sink\n");
        return 1;
    }
    ag_pcm_set_chunk(&s_out, CHUNK);
    ag_printf("synth: sound = %s @ %u Hz\n", s_out.path, (unsigned)RATE);
    {
        const char *p = s_out.path;
        int n = 0;
        while (p[n]) {
            n++;
        }
        if (n > 4 && (p[n - 1] == 'v' || p[n - 1] == 'V')) {
            s_wav_until = ag_millis() + 4500u;
            ag_synth_set(&s_s, AG_SYNTH_P_DRIVE, 56);
            ag_synth_set(&s_s, AG_SYNTH_P_DIST_MODEL, AG_DIST_TUBE);
            s_ir_on = 1;
            ag_synth_note_on(&s_s, 64, 110);
        } else {
            open_midivirt();
        }
    }

    if (ag_fx_init(&s_fx, RATE) == 0) {
        s_fx_ok = 1;
        ag_fx_set_defaults(&s_fx);
        s_fx.master_wet = 50;
    }
    /*
     * No ag_ir_set_wet here: s_cab is a cabinet, ag_ir_load_preset takes those
     * fully wet, and the 90 of 128 that used to be set here let a third of the
     * dry signal past the cabinet with its top end intact.  The 'c' key
     * reloads the preset and would not have restored the 90 anyway.
     */
    if (ag_ir_init(&s_ir, RATE) == 0 && ag_ir_load_preset(&s_ir, s_cab) == 0) {
        s_ir_ok = 1;
    }

    ag_cursor(false);
    draw_ui();
    s_ui_ms = ag_millis();
    ag_pcm_pace_start(&s_out);

    while (s_running) {
        ag_time_t t0;
        if (s_wav_until != 0u && (int32_t)(ag_millis() - s_wav_until) >= 0) {
            break;
        }
        while (ag_poll_event(&ev, 0)) {
            if (ev.type == AG_EV_QUIT) {
                if (s_wav_until == 0u) {
                    s_running = 0;
                    break;
                }
            }
            if (ev.type == AG_EV_KEY_DOWN || ev.type == AG_EV_KEY_UP) {
                handle_key((int)ev.key.keycode, ev.type == AG_EV_KEY_DOWN,
                           ev.key.unicode);
            }
        }
        if (!s_running) {
            break;
        }

        pump_midivirt();
        t0 = ag_micros();
        ag_synth_render(&s_s, s_pcm, (int32_t)CHUNK);
        if (s_fx_ok && s_fx_on) {
            ag_fx_process(&s_fx, s_pcm, (int32_t)CHUNK);
        }
        apply_ir(s_pcm, (int32_t)CHUNK);
        ag_pcm_mark_render(&s_out, (uint32_t)(ag_micros() - t0));
        (void)ag_pcm_write(&s_out, s_pcm, (int32_t)CHUNK);

        if (s_dirty || (ag_millis() - s_ui_ms) >= 250u) {
            if (ag_pcm_slack_us(&s_out) > 4000) {
                s_ui_ms = ag_millis();
                s_dirty = 1;
                draw_ui();
            }
        }
        ag_pcm_pace_wait(&s_out);
        ag_heartbeat();
    }

    ag_synth_all_notes_off(&s_s);
    if (s_fx_ok) {
        ag_fx_free(&s_fx);
    }
    if (s_ir_ok) {
        ag_ir_free(&s_ir);
    }
    if (s_midi_fd >= 0) {
        (void)ag_dev_close(s_midi_fd);
        s_midi_fd = -1;
    }
    ag_pcm_close(&s_out);
    ag_cursor(true);
    ag_cls();
    return 0;
}
