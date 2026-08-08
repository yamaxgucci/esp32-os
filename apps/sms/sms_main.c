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

AG_APP_SIZED("SMS", "1.0", "argon", AG_AXE_NEEDS_GFX, 32 * 1024, 2 * 1024 * 1024);

/* Built-in tiny ROM: clear VRAM-ish loop so a missing cart still draws something. */
static const uint8_t k_tiny_rom[0x4000] = {
    0xF3,                   /* di */
    0x31, 0xF0, 0xDF,       /* ld sp,$DFF0 */
    0x18, 0xFE,             /* jr $ */
};

static uint16_t s_frame[VIDEO_WIDTH_SMS * VIDEO_HEIGHT_SMS];

/* Terminals send KEY_DOWN only; hold bits for a few frames / until refreshed. */
static uint8_t s_pad_ttl;
static uint8_t s_pause_ttl;

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

static void poll_pad(void)
{
    ag_event_t ev;
    while (ag_poll_event(&ev, 0)) {
        if (ev.type != AG_EV_KEY_DOWN && ev.type != AG_EV_KEY_UP) {
            continue;
        }
        const bool down = (ev.type == AG_EV_KEY_DOWN);
        uint8_t    bit = 0;
        switch (ev.key.keycode) {
        case AG_KEY_UP:
        case AG_KEY_W: bit = INPUT_UP; break;
        case AG_KEY_DOWN:
        case AG_KEY_S: bit = INPUT_DOWN; break;
        case AG_KEY_LEFT:
        case AG_KEY_A: bit = INPUT_LEFT; break;
        case AG_KEY_RIGHT:
        case AG_KEY_D: bit = INPUT_RIGHT; break;
        case AG_KEY_Z:
        case AG_KEY_J:
        case AG_KEY_SPACE: bit = INPUT_BUTTON1; break;
        case AG_KEY_X:
        case AG_KEY_K: bit = INPUT_BUTTON2; break;
        case AG_KEY_ENTER:
        case AG_KEY_P:
            /*
             * SMS "Start" is the console Pause button (NMI). INPUT_START is
             * Game Gear only — mapping Enter there did nothing on SMS carts.
             */
            if (down) {
                s_pause_ttl = 3;
            }
            continue;
        case AG_KEY_ESC:
        case AG_KEY_Q:
            if (down) {
                ag_exit(0);
            }
            continue;
        default: continue;
        }
        if (down) {
            input.pad[0] |= bit;
            s_pad_ttl = 12; /* ~200 ms at 60 fps; covers no KEY_UP from UART */
        } else {
            input.pad[0] &= (uint8_t)~bit;
        }
    }

    if (s_pause_ttl > 0u) {
        input.system |= (uint8_t)INPUT_PAUSE;
        s_pause_ttl--;
    } else {
        input.system &= (uint8_t)~INPUT_PAUSE;
    }

    if (s_pad_ttl > 0u) {
        s_pad_ttl--;
    } else {
        input.pad[0] = 0;
    }
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

    if (argc > 1 && argv[1] != NULL) {
        if (argc > 2) {
            rom = argv[1];
            frames = parse_frames(argv[2], -1);
        } else if (looks_like_frames_only(argv[1])) {
            frames = parse_frames(argv[1], 180);
        } else {
            rom = argv[1];
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
    ag_gfx_release();
    system_poweroff();
    return 0;
}
