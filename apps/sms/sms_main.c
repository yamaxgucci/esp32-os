/*
 * ArgonOS - mute Master System player (SMS Plus GX core).
 *
 *   python tools/mkaxe.py ... -o SMS.AXE (see apps/sms/README.md)
 *   run t:\sms.axe t:\rom.sms
 *
 * Core: GPLv2+ (SMS Plus GX).  This file: Apache-2.0.
 */
#include <argon/argon.h>
#include <argon/keys.h>

#include "shared.h"
#include "sms_cfg.h"

AG_APP_SIZED("SMS", "1.0", "argon", AG_AXE_NEEDS_GFX, 32 * 1024, 2 * 1024 * 1024);

/* Built-in tiny ROM: clear VRAM-ish loop so a missing cart still draws something. */
static const uint8_t k_tiny_rom[0x4000] = {
    0xF3,                   /* di */
    0x31, 0xF0, 0xDF,       /* ld sp,$DFF0 */
    0x18, 0xFE,             /* jr $ */
};

static uint16_t s_frame[VIDEO_WIDTH_SMS * VIDEO_HEIGHT_SMS];
static sms_cfg_t s_cfg;

/*
 * Preferred input: host-pushed live pad (H:\sms.pad via HostFS PADPUSH).
 * That is level-state Win32 keys — diagonals and move+fire work, no UART
 * round-trip per frame (guest reads a RAM cache).
 *
 * Fallback: serial KEY_DOWN sticky keys.  Terminals have no KEY_UP and OS
 * autorepeat only refreshes the last key; we keep bits for PAD_HOLD_MS after
 * any mapped KEY_DOWN (refreshes all currently held bits).
 */
static ag_handle_t s_host_pad = -1;
static int         s_use_host_pad;

#define PAD_HOLD_MS 1000u
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

static void blit_to_fb(ag_gfxinfo_t *info)
{
    const int ox = ((int)info->width - VIDEO_WIDTH_SMS) / 2;
    const int oy = ((int)info->height - VIDEO_HEIGHT_SMS) / 2;
    uint16_t *dst = (uint16_t *)info->fb;

    for (int y = 0; y < VIDEO_HEIGHT_SMS; y++) {
        const int dy = oy + y;
        if ((unsigned)dy >= info->height) {
            continue;
        }
        uint16_t *row = dst + dy * (info->stride / 2) + (ox > 0 ? ox : 0);
        const uint16_t *src = s_frame + y * VIDEO_WIDTH_SMS;
        const int copy_w = VIDEO_WIDTH_SMS;
        for (int x = 0; x < copy_w; x++) {
            if ((unsigned)(ox + x) < info->width) {
                row[x] = src[x];
            }
        }
    }
}

static void try_open_host_pad(void)
{
    /* Host starts PADPUSH on connect; wait briefly for the first snapshot. */
    for (int i = 0; i < 40; i++) {
        s_host_pad = ag_open("h:\\sms.pad", AG_O_RDONLY);
        if (s_host_pad >= 0) {
            s_use_host_pad = 1;
            ag_printf("sms: live pad H:\\sms.pad (host push)\n");
            return;
        }
        ag_delay(25);
    }
    ag_printf("sms: no H:\\sms.pad — serial sticky keys\n");
}

static int poll_host_pad(uint8_t *pad0, uint8_t *pad1, uint8_t *sys)
{
    if (!s_use_host_pad || s_host_pad < 0) {
        return 0;
    }
    /*
     * Guest VFS treats the file as size 3; after one read pos==EOF.  Rewind
     * each frame.  Reads hit the PADPUSH RAM cache (no UART round-trip).
     */
    if (ag_seek(s_host_pad, 0, AG_SEEK_SET) < 0) {
        return 0;
    }
    uint8_t buf[3];
    const int32_t n = ag_read(s_host_pad, buf, 3);
    if (n < 2) {
        return 0;
    }
    *pad0 = buf[0];
    *pad1 = buf[1];
    *sys = (n >= 3) ? buf[2] : 0;
    return 1;
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

static void poll_pad_events(void)
{
    ag_event_t ev;
    while (ag_poll_event(&ev, 0)) {
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

static void poll_pad(void)
{
    uint8_t host0 = 0;
    uint8_t host1 = 0;
    uint8_t host_sys = 0;
    int     got_host = 0;

    if (s_use_host_pad) {
        got_host = poll_host_pad(&host0, &host1, &host_sys);
        if (!got_host) {
            s_use_host_pad = 0;
        }
    }

    if (got_host) {
        /* Level state from host — replace sticky bits for both pads. */
        input.pad[0] = host0;
        input.pad[1] = host1;
        if ((host_sys & 1u) != 0u) {
            input.system |= (uint8_t)INPUT_PAUSE;
        } else {
            input.system &= (uint8_t)~INPUT_PAUSE;
        }
        if ((host_sys & 2u) != 0u) {
            ag_exit(0);
        }
        /* Still drain serial so Esc/Quit from terminal works if mapped. */
        ag_event_t ev;
        while (ag_poll_event(&ev, 0)) {
            if (ev.type != AG_EV_KEY_DOWN) {
                continue;
            }
            int pad = 0;
            int act = 0;
            if (sms_cfg_lookup(&s_cfg, ev.key.keycode, &pad, &act) &&
                act == SMS_ACT_QUIT) {
                ag_exit(0);
            }
        }
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

int ag_main(int argc, char **argv)
{
    const char *rom = NULL;
    /* <0 = run until Esc/Q. Explicit N frames still works for benches. */
    int         frames = -1;
    int         want_livepad = 1; /* default: try host push pad */
    int         force_sticky = 0;

    for (int i = 1; i < argc; i++) {
        if (arg_is_nolivepad(argv[i])) {
            force_sticky = 1;
            want_livepad = 0;
            continue;
        }
        if (arg_is_livepad(argv[i])) {
            want_livepad = 1;
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
    option.fm = 0;
    option.nosound = 1;
    option.spritelimit = 1;

    sms_bitmap = s_frame;
    memset(s_frame, 0, sizeof(s_frame));
    memset(&bitmap, 0, sizeof(bitmap));
    bitmap.data = (uint8_t *)s_frame;
    bitmap.width = VIDEO_WIDTH_SMS;
    bitmap.height = VIDEO_HEIGHT_SMS;
    bitmap.pitch = VIDEO_WIDTH_SMS * 2;
    bitmap.depth = 16;
    bitmap.viewport.w = VIDEO_WIDTH_SMS;
    bitmap.viewport.h = VIDEO_HEIGHT_SMS;

    sms_cfg_load(&s_cfg, rom);
    if (want_livepad && !force_sticky) {
        try_open_host_pad();
    } else {
        ag_printf("sms: controls = serial sticky\n");
    }

    if (!load_cart(rom)) {
        ag_printf("failed to load rom\n");
        return 1;
    }

    system_poweron();

    ag_gfxinfo_t info;
    if (ag_gfx_acquire(&info) != AG_OK) {
        ag_printf("gfx acquire failed\n");
        system_poweroff();
        return 1;
    }
    ag_gfx_clear(0x00000000u);
    int ran = 0;
    for (; frames < 0 || ran < frames; ran++) {
        const uint32_t t0 = ag_millis();
        poll_pad();
        system_frame(0);
        blit_to_fb(&info);
        /* Every frame: soft fb → QEMU RGB / fbcon / gfxdump /live. */
        ag_gfx_flush(0, 0, info.width, info.height);
        /* Cap ~60 fps (without QEMU RGB busy-wait the guest ran away). */
        ag_yield();
        const uint32_t dt = ag_millis() - t0;
        if (dt < 16u) {
            ag_delay(16u - dt);
        }
    }
    ag_printf("sms: %d frames, cart %u KB crc=%08x\n", ran,
              (unsigned)(cart.size / 1024u), (unsigned)cart.crc);
    if (s_host_pad >= 0) {
        ag_close(s_host_pad);
    }
    ag_gfx_release();
    system_poweroff();
    return 0;
}
