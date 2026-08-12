/*
 * ArgonOS - Master System player (SMS Plus GX core).
 *
 *   python tools/mkaxe.py ... -o SMS.AXE (see apps/sms/README.md)
 *   run t:\sms.axe t:\rom.sms
 *   run t:\sms.axe 60 nolivepad wav   # PSG → t:\sms.wav
 *   run t:\sms.axe 60 nolivepad mock  # PSG, discard samples
 *
 * Core: GPLv2+ (SMS Plus GX).  This file: Apache-2.0.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>
#include <argon/keys.h>

#include "shared.h"
#include "sms_cfg.h"
#include "sound_output.h"

AG_APP_SIZED("SMS", "1.0", "argon", AG_AXE_NEEDS_GFX, 32 * 1024, 2 * 1024 * 1024);

/* Built-in tiny ROM: clear VRAM-ish loop so a missing cart still draws something. */
static const uint8_t k_tiny_rom[0x4000] = {
    0xF3,                   /* di */
    0x31, 0xF0, 0xDF,       /* ld sp,$DFF0 */
    0x18, 0xFE,             /* jr $ */
};

static sms_cfg_t s_cfg;

/*
 * The emulator renders straight into the gfx back buffer: bitmap.pitch is the
 * display stride and bitmap.data points at where line 0 lands.  There is no
 * intermediate frame and no blit - the old path copied 96 KB per frame for
 * nothing, and then flushed all 640x400 on top of that.
 *
 * Room for 224 lines is reserved even though most games run 192, because the
 * VDP can switch to the taller mode at runtime while the origin cannot move:
 * render_reset() clears pitch x height bytes from it once, at power-on.
 */
#define SMS_MAX_LINES 224
static int s_ox;
static int s_oy;

/*
 * Preferred input: kernel pad layer (HostFS PADPUSH → inp->btnp).  Real level
 * state, so diagonals and move+fire work with no per-frame file I/O.
 *
 * `nolivepad` forces serial KEY_DOWN sticky keys.  Terminals have no KEY_UP,
 * so that path is a degraded reserve only.
 */
static int s_use_live_pad = 1;
static int s_quit;

#define PAD_HOLD_MS 150u /* matches console sticky TTL */
static uint32_t s_pad_until[2][6]; /* millis deadline per bit */
static uint8_t  s_pause_ttl;

static const uint8_t k_act_bit[SMS_ACT_COUNT] = {
    INPUT_UP, INPUT_DOWN, INPUT_LEFT, INPUT_RIGHT,
    INPUT_BUTTON1, INPUT_BUTTON2, 0, 0,
};

static int arg_is_livepad(const char *s)
{
    if (s == NULL) {
        return 0;
    }
    const char *a = "livepad";
    for (; *a && *s; a++, s++) {
        char ca = *a;
        char cb = *s;
        if (cb >= 'A' && cb <= 'Z') {
            cb = (char)(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return 0;
        }
    }
    return *a == '\0' && *s == '\0';
}

static int arg_is_nolivepad(const char *s)
{
    if (s == NULL) {
        return 0;
    }
    const char *a = "nolivepad";
    for (; *a && *s; a++, s++) {
        char ca = *a;
        char cb = *s;
        if (cb >= 'A' && cb <= 'Z') {
            cb = (char)(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return 0;
        }
    }
    return *a == '\0' && *s == '\0';
}

static int arg_is_mock(const char *s)
{
    if (s == NULL) {
        return 0;
    }
    const char *a = "mock";
    for (; *a && *s; a++, s++) {
        char ca = *a;
        char cb = *s;
        if (cb >= 'A' && cb <= 'Z') {
            cb = (char)(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return 0;
        }
    }
    return *a == '\0' && *s == '\0';
}

static int arg_is_sound(const char *s)
{
    if (s == NULL) {
        return 0;
    }
    const char *a = "sound";
    for (; *a && *s; a++, s++) {
        char ca = *a;
        char cb = *s;
        if (cb >= 'A' && cb <= 'Z') {
            cb = (char)(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return 0;
        }
    }
    return *a == '\0' && *s == '\0';
}

static int arg_is_wav(const char *s)
{
    if (s == NULL) {
        return 0;
    }
    const char *a = "wav";
    for (; *a && *s; a++, s++) {
        char ca = *a;
        char cb = *s;
        if (cb >= 'A' && cb <= 'Z') {
            cb = (char)(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return 0;
        }
    }
    return *a == '\0' && *s == '\0';
}

static int arg_eq(const char *s, const char *lit)
{
    if (s == NULL) {
        return 0;
    }
    for (; *lit && *s; lit++, s++) {
        char cb = *s;
        if (cb >= 'A' && cb <= 'Z') {
            cb = (char)(cb - 'A' + 'a');
        }
        if (*lit != cb) {
            return 0;
        }
    }
    return *lit == '\0' && *s == '\0';
}

/*
 * "% realtime" is the number that decides whether this is playable: one NTSC
 * frame is 16667 us of guest time, so spending that much on it means keeping up
 * exactly.  Everything here is wall time on the emulator's own work - the pacing
 * sleep is deliberately outside it.
 */
static void report_stats(const char *tag, uint32_t frames, uint64_t span_us,
                         uint64_t work_sum, uint64_t emu_sum,
                         uint64_t present_sum, uint64_t work_max)
{
    if (frames == 0u || span_us == 0u) {
        return;
    }
    const unsigned avg = (unsigned)(work_sum / frames);
    const unsigned emu = (unsigned)(emu_sum / frames);
    const unsigned pres = (unsigned)(present_sum / frames);
    const unsigned fps = (unsigned)((uint64_t)frames * 1000000u / span_us);
    const unsigned pct = (avg > 0u) ? (unsigned)(1666700u / avg) : 0u;
    ag_printf("%s: %u fps, work %u us (emu %u, show %u), max %u us, "
              "%u%% realtime\n",
              tag, fps, avg, emu, pres, (unsigned)work_max, pct);
}

static int load_cart(const char *path)
{
    if (path == NULL || path[0] == '\0') {
        return (int)load_rom_mem((const char *)k_tiny_rom, sizeof(k_tiny_rom));
    }

    const ag_handle_t h = ag_open(path, AG_O_RDONLY);
    if (h < 0) {
        ag_printf("rom open failed: %s\n", path);
        return (int)load_rom_mem((const char *)k_tiny_rom, sizeof(k_tiny_rom));
    }

    const int64_t sz = ag_seek(h, 0, AG_SEEK_END);
    ag_seek(h, 0, AG_SEEK_SET);
    if (sz <= 0 || sz > 4 * 1024 * 1024) {
        ag_close(h);
        return 0;
    }

    uint8_t *buf = (uint8_t *)ag_malloc((size_t)sz);
    if (buf == NULL) {
        ag_close(h);
        return 0;
    }
    size_t got = 0;
    while (got < (size_t)sz) {
        const int32_t n = ag_read(h, buf + got, (size_t)sz - got);
        if (n <= 0) {
            break;
        }
        got += (size_t)n;
    }
    ag_close(h);

    const int ok = (int)load_rom_mem((const char *)buf, got);
    ag_free(buf);
    return ok;
}

/* Aim the emulator at the acquired framebuffer.  Must run before
 * system_poweron(), which clears the bitmap through these fields. */
static void bind_frame_to_fb(const ag_gfxinfo_t *info)
{
    s_ox = ((int)info->width > VIDEO_WIDTH_SMS)
               ? ((int)info->width - VIDEO_WIDTH_SMS) / 2
               : 0;
    s_oy = ((int)info->height > SMS_MAX_LINES)
               ? ((int)info->height - SMS_MAX_LINES) / 2
               : 0;

    uint8_t *const origin = (uint8_t *)info->fb + (size_t)s_oy * info->stride +
                            (size_t)s_ox * sizeof(uint16_t);

    sms_bitmap = (uint16_t *)origin;
    memset(&bitmap, 0, sizeof(bitmap));
    bitmap.data = origin;
    bitmap.width = VIDEO_WIDTH_SMS;
    bitmap.height = VIDEO_HEIGHT_SMS;
    bitmap.pitch = (int32_t)info->stride;
    bitmap.depth = 16;
    bitmap.viewport.w = VIDEO_WIDTH_SMS;
    bitmap.viewport.h = VIDEO_HEIGHT_SMS;
}

static void refresh_held(int pad)
{
    const uint32_t until = ag_millis() + PAD_HOLD_MS;
    for (unsigned i = 0; i < 6u; i++) {
        if ((input.pad[pad] & (uint8_t)(1u << i)) != 0u) {
            s_pad_until[pad][i] = until;
        }
    }
}

static void apply_action(int pad, int act, bool down)
{
    if (act == SMS_ACT_PAUSE) {
        if (down) {
            s_pause_ttl = 3;
        }
        return;
    }
    if (act == SMS_ACT_QUIT) {
        if (down) {
            ag_exit(0);
        }
        return;
    }
    if (act < 0 || act > SMS_ACT_B2 || pad < 0 || pad > 1) {
        return;
    }

    const uint8_t bit = k_act_bit[act];
    unsigned      idx = 0;
    for (uint8_t t = bit; t > 1u; t >>= 1) {
        idx++;
    }
    if (down) {
        /* Opposing directions cancel so Left+Right cannot both stick. */
        if (bit == INPUT_UP) {
            input.pad[pad] &= (uint8_t)~INPUT_DOWN;
            s_pad_until[pad][1] = 0;
        } else if (bit == INPUT_DOWN) {
            input.pad[pad] &= (uint8_t)~INPUT_UP;
            s_pad_until[pad][0] = 0;
        } else if (bit == INPUT_LEFT) {
            input.pad[pad] &= (uint8_t)~INPUT_RIGHT;
            s_pad_until[pad][3] = 0;
        } else if (bit == INPUT_RIGHT) {
            input.pad[pad] &= (uint8_t)~INPUT_LEFT;
            s_pad_until[pad][2] = 0;
        }
        input.pad[pad] |= bit;
        /* Autorepeat only refreshes the last key — keep every held bit alive. */
        refresh_held(pad);
        if (idx < 6u) {
            s_pad_until[pad][idx] = ag_millis() + PAD_HOLD_MS;
        }
    } else {
        input.pad[pad] &= (uint8_t)~bit;
        if (idx < 6u) {
            s_pad_until[pad][idx] = 0;
        }
    }
}

/* FOCUS_GAINED: reclaim gfx after the kernel force-released it. */
static int handle_session_ev(const ag_event_t *ev)
{
    if (ev->type == AG_EV_FOCUS_GAINED) {
        ag_gfxinfo_t info;
        if (ag_gfx_acquire(&info) == AG_OK) {
            bind_frame_to_fb(&info);
        }
        return 1;
    }
    if (ev->type == AG_EV_FOCUS_LOST) {
        return 1;
    }
    if (ev->type == AG_EV_QUIT) {
        if (ag_focused()) {
            s_quit = 1;
        }
        return 1;
    }
    return 0;
}

static void poll_pad_events(void)
{
    ag_event_t ev;
    while (ag_poll_event(&ev, 0)) {
        if (handle_session_ev(&ev)) {
            continue;
        }
        if (!ag_focused()) {
            continue;
        }
        if (ev.type != AG_EV_KEY_DOWN && ev.type != AG_EV_KEY_UP) {
            continue;
        }
        int pad = 0;
        int act = 0;
        if (!sms_cfg_lookup(&s_cfg, ev.key.keycode, &pad, &act)) {
            continue;
        }
        apply_action(pad, act, ev.type == AG_EV_KEY_DOWN);
    }

    if (s_pause_ttl > 0u) {
        input.system |= (uint8_t)INPUT_PAUSE;
        s_pause_ttl--;
    } else {
        input.system &= (uint8_t)~INPUT_PAUSE;
    }

    const uint32_t now = ag_millis();
    for (int pad = 0; pad < 2; pad++) {
        for (unsigned i = 0; i < 6u; i++) {
            if (s_pad_until[pad][i] == 0u) {
                continue;
            }
            if ((int32_t)(now - s_pad_until[pad][i]) >= 0) {
                s_pad_until[pad][i] = 0;
                input.pad[pad] &= (uint8_t)~(1u << i);
            }
        }
    }
}

static void poll_pad_live(void)
{
    static const int     k_ids[6] = {AG_BTN_UP,    AG_BTN_DOWN, AG_BTN_LEFT,
                                     AG_BTN_RIGHT, AG_BTN_B1,   AG_BTN_B2};
    static const uint8_t k_bits[6] = {INPUT_UP,    INPUT_DOWN, INPUT_LEFT,
                                      INPUT_RIGHT, INPUT_BUTTON1, INPUT_BUTTON2};
    ag_event_t ev;
    while (ag_poll_event(&ev, 0)) {
        (void)handle_session_ev(&ev);
    }
    if (!ag_focused()) {
        input.pad[0] = 0;
        input.pad[1] = 0;
        input.system &= (uint8_t)~(INPUT_PAUSE);
        return;
    }
    uint8_t p0 = 0;
    uint8_t p1 = 0;
    for (unsigned i = 0; i < 6u; i++) {
        if (ag_btnp(0, k_ids[i])) {
            p0 |= k_bits[i];
        }
        if (ag_btnp(1, k_ids[i])) {
            p1 |= k_bits[i];
        }
    }
    input.pad[0] = p0;
    input.pad[1] = p1;
    if (ag_btnp(0, AG_BTN_PAUSE) || ag_btnp(1, AG_BTN_PAUSE)) {
        input.system |= (uint8_t)INPUT_PAUSE;
    } else {
        input.system &= (uint8_t)~INPUT_PAUSE;
    }
    if (ag_btnp(0, AG_BTN_QUIT) || ag_btnp(1, AG_BTN_QUIT)) {
        s_quit = 1;
    }
}

static void poll_pad(void)
{
    if (s_use_live_pad) {
        poll_pad_live();
        return;
    }
    poll_pad_events();
}

static int parse_frames(const char *s, int fallback)
{
    if (s == NULL || s[0] < '0' || s[0] > '9') {
        return fallback;
    }
    int frames = 0;
    for (const char *p = s; *p >= '0' && *p <= '9'; p++) {
        frames = frames * 10 + (*p - '0');
    }
    return frames > 0 ? frames : fallback;
}

static int looks_like_frames_only(const char *s)
{
    if (s == NULL || s[0] == '\0') {
        return 0;
    }
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') {
            return 0;
        }
    }
    return 1;
}

static int looks_like_wav_path(const char *s)
{
    size_t n = 0;
    if (s == NULL) {
        return 0;
    }
    while (s[n]) {
        n++;
    }
    if (n < 5) {
        return 0;
    }
    return (s[n - 4] == '.') &&
           (s[n - 3] == 'w' || s[n - 3] == 'W') &&
           (s[n - 2] == 'a' || s[n - 2] == 'A') &&
           (s[n - 1] == 'v' || s[n - 1] == 'V');
}

int ag_main(int argc, char **argv)
{
    const char *rom = NULL;
    /* <0 = run until Esc/Q. Explicit N frames still works for benches. */
    int         frames = -1;
    int         want_livepad = 1; /* default: try host push pad */
    int         force_sticky = 0;
    int         want_sound = 0;
    const char *wav_path = "mock";
    int         present_div = 1; /* frames of emulation per frame shown */
    int         stats = 0;

    for (int i = 1; i < argc; i++) {
        if (arg_eq(argv[i], "fps30")) {
            present_div = 2;
            continue;
        }
        if (arg_eq(argv[i], "fps60")) {
            present_div = 1;
            continue;
        }
        if (arg_eq(argv[i], "stats")) {
            stats = 1;
            continue;
        }
        if (arg_is_nolivepad(argv[i])) {
            force_sticky = 1;
            want_livepad = 0;
            continue;
        }
        if (arg_is_livepad(argv[i])) {
            want_livepad = 1;
            continue;
        }
        if (arg_is_mock(argv[i]) || arg_is_sound(argv[i])) {
            want_sound = 1;
            wav_path = "pcmnull";
            continue;
        }
        if (arg_eq(argv[i], "pcmnull") || arg_eq(argv[i], "pcmvirt") ||
            arg_eq(argv[i], "pcmmix") || arg_eq(argv[i], "mix") ||
            arg_eq(argv[i], "net") || arg_eq(argv[i], "tcp") ||
            arg_eq(argv[i], "audio") || arg_eq(argv[i], "i2s") ||
            arg_eq(argv[i], "pcm0")) {
            want_sound = 1;
            wav_path = argv[i];
            continue;
        }
        if (arg_is_wav(argv[i])) {
            want_sound = 1;
            if (!looks_like_wav_path(wav_path)) {
                wav_path = "t:\\sms.wav";
            }
            continue;
        }
        if (looks_like_wav_path(argv[i]) ||
            (argv[i][0] == '/' ||
             ((argv[i][0] == 'd' || argv[i][0] == 'D') && argv[i][1] == ':'))) {
            want_sound = 1;
            wav_path = argv[i];
            continue;
        }
        if (rom == NULL && !looks_like_frames_only(argv[i])) {
            rom = argv[i];
            continue;
        }
        if (looks_like_frames_only(argv[i])) {
            frames = parse_frames(argv[i], -1);
        }
    }

    memset(&option, 0, sizeof(option));
    option.console = 2; /* SMS2 (tiny built-in ROM has no TMR SEGA header) */
    option.country = 1; /* USA / NTSC */
    option.fm = want_sound ? 1 : 0; /* OPLL regs → ag_fm lite synth */
    option.nosound = want_sound ? 0 : 1;
    option.soundlevel = 2;
    option.spritelimit = 1;

    /* Graphics first: the emulator draws into the acquired buffer, and
     * system_poweron() already clears it through bitmap.*. */
    ag_gfxinfo_t info;
    if (ag_gfx_acquire(&info) != AG_OK) {
        ag_printf("gfx acquire failed\n");
        return 1;
    }
    ag_gfx_clear(0x00000000u);
    bind_frame_to_fb(&info);

    sms_cfg_load(&s_cfg, rom);
    s_use_live_pad = (want_livepad && !force_sticky) ? 1 : 0;
    if (s_use_live_pad) {
        ag_printf("sms: controls = live pad (inp / HostFS PADPUSH)\n");
    } else {
        ag_printf("sms: controls = serial sticky (no key-up)\n");
    }

    if (!load_cart(rom)) {
        ag_printf("failed to load rom\n");
        ag_gfx_release();
        return 1;
    }

    if (want_sound) {
        Sound_SetPath(wav_path);
    }
    system_poweron();
    if (want_sound) {
        Sound_Init();
    }

    ag_printf("sms: %ux%u at %d,%d, present every %d frame(s)\n",
              (unsigned)VIDEO_WIDTH_SMS, (unsigned)VIDEO_HEIGHT_SMS, s_ox, s_oy,
              present_div);

    /*
     * Emulation always runs at the guest's 60 Hz - the Z80 and the sound chips
     * have to, or the game plays slowly rather than less smoothly.  The divider
     * only skips rasterising and showing, which is where the cost is.
     */
    uint64_t work_sum = 0; /* emu + present, no pacing sleep */
    uint64_t emu_sum = 0;
    uint64_t present_sum = 0;
    uint64_t work_max = 0;
    uint32_t window = 0;
    ag_time_t window_t0 = ag_micros();
    int       ran = 0;

    for (; !s_quit && (frames < 0 || ran < frames);) {
        poll_pad();
        if (s_quit) {
            break;
        }
        if (!ag_focused()) {
            ag_heartbeat();
            ag_delay(50);
            continue;
        }

        const uint32_t  t0 = ag_millis();
        const ag_time_t f0 = ag_micros();

        const int show = (present_div <= 1) || ((ran % present_div) == 0);
        system_frame(show ? 0 : 1);

        const ag_time_t f1 = ag_micros();
        if (show) {
            ag_gfx_flush((uint16_t)s_ox, (uint16_t)s_oy, VIDEO_WIDTH_SMS,
                         (uint16_t)bitmap.viewport.h);
        }
        const ag_time_t f2 = ag_micros();

        const uint64_t work = (uint64_t)(f2 - f0);
        work_sum += work;
        emu_sum += (uint64_t)(f1 - f0);
        present_sum += (uint64_t)(f2 - f1);
        if (work > work_max) {
            work_max = work;
        }
        window++;
        ran++;

        if (stats && (uint64_t)(f2 - window_t0) >= 2000000u) {
            report_stats("sms", window, (uint64_t)(f2 - window_t0), work_sum,
                         emu_sum, present_sum, work_max);
            work_sum = emu_sum = present_sum = work_max = 0;
            window = 0;
            window_t0 = f2;
        }

        /* Cap ~60 fps (without QEMU RGB busy-wait the guest ran away). */
        ag_yield();
        const uint32_t dt = ag_millis() - t0;
        if (dt < 16u) {
            ag_delay(16u - dt);
        }
    }
    if (window > 0) {
        report_stats("sms", window, (uint64_t)(ag_micros() - window_t0),
                     work_sum, emu_sum, present_sum, work_max);
    }
    ag_printf("sms: %d frames, cart %u KB crc=%08x\n", ran,
              (unsigned)(cart.size / 1024u), (unsigned)cart.crc);
    Sound_Close();
    ag_gfx_release();
    system_poweroff();
    return 0;
}
