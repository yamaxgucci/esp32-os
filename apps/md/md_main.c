/*
 * ArgonOS - Mega Drive player (gwenesis cores), mute milestone.
 *
 *   python tools/mkaxe.py ... -o MD.AXE (see apps/md/README.md)
 *   run a:\MD.AXE a:\game.bin
 *   run a:\MD.AXE 600 stats          # bench the 68000 + VDP path
 *   run a:\MD.AXE a:\game.bin fps30  # emulate 60, show 30
 *
 * Cores: gwenesis, GPLv3 (see LICENSE and README).  This file: Apache-2.0.
 */
#include <argon/argon.h>
#include <argon/keys.h>

#include "gwenesis_bus.h"
#include "gwenesis_io.h"
#include "gwenesis_vdp.h"
#include "m68k.h"
#include "md_sound.h"
#include "ym2612.h"
#include "z80inst.h"

/* 24 KB stack: with OpenEth/lwIP the largest internal free block is ~31 KB. */
AG_APP_SIZED("MD", "0.1", "argon", AG_AXE_NEEDS_GFX, 24 * 1024, 5 * 1024 * 1024);

/*
 * The cores expect these two to live in the platform: every logging site and the
 * VDP's own timing take the current line from here.
 */
int scan_line;
int frame_counter;

extern int            hint_pending;
extern unsigned short gwenesis_vdp_status;
extern unsigned char  gwenesis_vdp_regs[];
extern int            screen_width;
extern int            screen_height;

/* Added to the vendored core; see README, "Local changes". */
extern void gwenesis_vdp_set_buffer_stride(int pixels);

/*
 * Like the Master System player, the emulator renders straight into the gfx back
 * buffer: gwenesis' embedded path writes finished RGB565 through CRAM565, so a
 * window of the framebuffer *is* the screen and there is no frame copy at all.
 * The stride hook is what makes the window possible - upstream steps by exactly
 * 320 pixels per line.
 *
 * Room for 240 lines is reserved even though most games run 224, because the VDP
 * can switch modes at runtime while the origin must not move.
 */
#define MD_WIDTH 320
#define MD_MAX_LINES 240
static int s_ox;
static int s_oy;

/*
 * Input: prefer the kernel pad layer (HostFS PADPUSH → inp->btnp).  That is
 * real level state, so directions and A+B+C can be held.  `nolivepad` forces
 * the serial sticky path, which has no key-up and is only a degraded reserve.
 */
static int s_use_live_pad = 1;
static int s_run_z80_cpu = 1;   /* 0: advance zclk only, no sms_z80_execute */
static int s_run_sound = 1;     /* 0: skip sample mix (still accepts reg writes) */
static int s_profile_frames = 0; /* print per-frame m68k/z80/vdp split */

/* Last run_frame phase times (µs), filled when s_profile_frames > 0. */
static uint32_t s_prof_m68k_us;
static uint32_t s_prof_z80_us;
static uint32_t s_prof_snd_us;
static uint32_t s_prof_vdp_us;

#define PAD_HOLD_MS 150u /* matches console sticky TTL; serial has no key-up */
#define MD_BUTTONS 8
static uint32_t s_pad_until[MD_BUTTONS];

static const struct {
    int ag;
    int md;
} k_live_map[] = {
    {AG_BTN_UP, PAD_UP}, {AG_BTN_DOWN, PAD_DOWN},
    {AG_BTN_LEFT, PAD_LEFT}, {AG_BTN_RIGHT, PAD_RIGHT},
    {AG_BTN_B1, PAD_A}, {AG_BTN_B2, PAD_B}, {AG_BTN_C, PAD_C},
    {AG_BTN_START, PAD_S},
};

static int key_to_pad(uint16_t keycode)
{
    switch (keycode) {
    case AG_KEY_UP:
        return PAD_UP;
    case AG_KEY_DOWN:
        return PAD_DOWN;
    case AG_KEY_LEFT:
        return PAD_LEFT;
    case AG_KEY_RIGHT:
        return PAD_RIGHT;
    case AG_KEY_Z:
        return PAD_A;
    case AG_KEY_X:
        return PAD_B;
    case AG_KEY_C:
        return PAD_C;
    case AG_KEY_ENTER:
        return PAD_S;
    default:
        return -1;
    }
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

static int looks_like_number(const char *s)
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

static int parse_int(const char *s, int fallback)
{
    if (!looks_like_number(s)) {
        return fallback;
    }
    int v = 0;
    for (const char *p = s; *p; p++) {
        v = v * 10 + (*p - '0');
    }
    return v > 0 ? v : fallback;
}

/*
 * "% realtime" against one NTSC frame of 16667 us: 100% means keeping up exactly.
 * The pacing sleep is deliberately outside the measured work.
 */
static void report_stats(uint32_t frames, uint64_t span_us, uint64_t work_sum,
                         uint64_t emu_sum, uint64_t present_sum,
                         uint64_t work_max)
{
    if (frames == 0u || span_us == 0u) {
        return;
    }
    const unsigned avg = (unsigned)(work_sum / frames);
    const unsigned emu = (unsigned)(emu_sum / frames);
    const unsigned pres = (unsigned)(present_sum / frames);
    const unsigned fps = (unsigned)((uint64_t)frames * 1000000u / span_us);
    const unsigned pct = (avg > 0u) ? (unsigned)(1666700u / avg) : 0u;
    ag_printf("md: %u fps, work %u us (emu %u, show %u), max %u us, "
              "%u%% realtime\n",
              fps, avg, emu, pres, (unsigned)work_max, pct);
}

/*
 * A cartridge with nothing in it but a reset vector and a branch to itself.  It
 * exists so the 68000 and the VDP can be benchmarked without a ROM on the share:
 * the CPU spins, the VDP still rasterises all 224 lines, and the number that
 * comes out is the cost of the display path plus an idle core.
 *
 * Written big-endian as a real cartridge is; load_cartridge does the byte swap
 * the fetch macros expect.
 */
static unsigned char *make_tiny_rom(size_t *size_out)
{
    const size_t sz = 0x400;
    unsigned char *rom = (unsigned char *)ag_malloc(sz);
    if (rom == NULL) {
        return NULL;
    }
    memset(rom, 0, sz);

    /* Initial supervisor stack pointer, then initial PC. */
    rom[0] = 0x00; rom[1] = 0xFF; rom[2] = 0x00; rom[3] = 0x00;
    rom[4] = 0x00; rom[5] = 0x00; rom[6] = 0x02; rom[7] = 0x00;

    /* 0x200: nop; bra.s *-2  - stay put without stopping the clock. */
    rom[0x200] = 0x4E; rom[0x201] = 0x71;
    rom[0x202] = 0x60; rom[0x203] = 0xFE;

    *size_out = sz;
    return rom;
}

static unsigned char *load_rom_file(const char *path, size_t *size_out)
{
    const ag_handle_t h = ag_open(path, AG_O_RDONLY);
    if (h < 0) {
        ag_printf("md: rom open failed: %s\n", path);
        return NULL;
    }
    const int64_t sz = ag_seek(h, 0, AG_SEEK_END);
    ag_seek(h, 0, AG_SEEK_SET);
    if (sz <= 0 || sz > 4 * 1024 * 1024) {
        ag_printf("md: rom size %d rejected\n", (int)sz);
        ag_close(h);
        return NULL;
    }

    /*
     * Rounded up to 64 KB: a game that reads past the end of its own cartridge
     * lands in this padding instead of past the allocation.
     */
    const size_t alloc = (((size_t)sz + 0xFFFFu) & ~(size_t)0xFFFFu);
    unsigned char *buf = (unsigned char *)ag_malloc(alloc);
    if (buf == NULL) {
        ag_printf("md: out of memory for %d KB rom\n", (int)(sz / 1024));
        ag_close(h);
        return NULL;
    }
    memset(buf + (size_t)sz, 0, alloc - (size_t)sz);

    size_t got = 0;
    while (got < (size_t)sz) {
        const int32_t n = ag_read(h, buf + got, (size_t)sz - got);
        if (n <= 0) {
            break;
        }
        got += (size_t)n;
    }
    ag_close(h);
    *size_out = got;
    return buf;
}

static void poll_pad_live(void)
{
    for (unsigned i = 0; i < sizeof(k_live_map) / sizeof(k_live_map[0]); i++) {
        if (ag_btnp(0, k_live_map[i].ag)) {
            gwenesis_io_pad_press_button(0, k_live_map[i].md);
        } else {
            gwenesis_io_pad_release_button(0, k_live_map[i].md);
        }
    }
    /* Old 3-byte hosts only have sys.pause; btnp(START) already ORs that in. */
    if (ag_btnp(0, AG_BTN_QUIT)) {
        ag_exit(0);
    }
}

static void poll_pad_serial(void)
{
    ag_event_t ev;
    while (ag_poll_event(&ev, 0)) {
        if (ev.type != AG_EV_KEY_DOWN && ev.type != AG_EV_KEY_UP) {
            continue;
        }
        if (ev.key.keycode == AG_KEY_ESC || ev.key.keycode == AG_KEY_Q) {
            ag_exit(0);
        }
        const int btn = key_to_pad(ev.key.keycode);
        if (btn < 0) {
            continue;
        }
        if (ev.type == AG_EV_KEY_DOWN) {
            gwenesis_io_pad_press_button(0, btn);
            s_pad_until[btn] = ag_millis() + PAD_HOLD_MS;
        } else {
            gwenesis_io_pad_release_button(0, btn);
            s_pad_until[btn] = 0;
        }
    }

    /* Serial has no reliable key-up, so held buttons expire on a timer. */
    const uint32_t now = ag_millis();
    for (int b = 0; b < MD_BUTTONS; b++) {
        if (s_pad_until[b] != 0u && (int32_t)(now - s_pad_until[b]) >= 0) {
            s_pad_until[b] = 0;
            gwenesis_io_pad_release_button(0, b);
        }
    }
}

static void poll_pad(void)
{
    if (s_use_live_pad) {
        poll_pad_live();
        return;
    }
    poll_pad_serial();
}

/* The cores call this when the 68000 reads the pad port; our state is already
 * pushed by poll_pad, so there is nothing to fetch. */
void gwenesis_io_get_buttons(void) {}

/*
 * One frame, line by line, on one core.  Every unit takes an absolute master
 * clock target, so advancing one counter keeps the 68000, Z80 and VDP in step
 * without either of them knowing about the other.
 *
 * `render` is what the display divider switches off: skipping the rasteriser is
 * where the saving is, and the 68000 has to run either way or the game slows
 * down instead of merely looking choppier.
 */
static void run_frame(int render)
{
    const int lines_per_frame = REG1_PAL ? LINES_PER_FRAME_PAL
                                         : LINES_PER_FRAME_NTSC;
    /*
     * hint_counter and system_clock must survive a longjmp from an odd
     * 68000 address: the trap is armed once below for the whole frame.
     */
    static int hint_counter;
    static int system_clock;
    const int  profile = s_profile_frames > 0;
    uint64_t   m68k_us = 0;
    uint64_t   z80_us = 0;
    uint64_t   snd_us = 0;
    uint64_t   vdp_us = 0;

    screen_width = REG12_MODE_H40 ? 320 : 256;
    screen_height = REG1_PAL ? VISIBLE_LINES_PAL : VISIBLE_LINES_NTSC;
    gwenesis_vdp_render_config();

    hint_counter = gwenesis_vdp_regs[10];
    system_clock = 0;
    scan_line = 0;
    if (s_run_sound) {
        md_sound_begin_frame(lines_per_frame);
    }

    if (m68k_arm_address_error_trap() != 0) {
        m68k_on_address_error();
        /* Resume the current line's m68k_run; do not re-add line cycles. */
        goto resume_m68k;
    }

    while (scan_line < lines_per_frame) {
        ag_time_t t0, t1;

        system_clock += VDP_CYCLES_PER_LINE;
resume_m68k:
        t0 = profile ? ag_micros() : 0;
        m68k_run(system_clock);
        if (profile) {
            t1 = ag_micros();
            m68k_us += (uint64_t)(t1 - t0);
            t0 = t1;
        }

        if (s_run_z80_cpu) {
            z80_run(system_clock);
        } else {
            /* Keep the Z80 clock in step so BUSREQ sync does not run away. */
            if (system_clock > zclk) {
                zclk = system_clock;
            }
        }
        if (profile) {
            t1 = ag_micros();
            z80_us += (uint64_t)(t1 - t0);
            t0 = t1;
        }

        if (s_run_sound) {
            /* Catch YM up to the line clock before draining samples. */
            ym2612_run(system_clock);
            md_sound_line(scan_line, lines_per_frame);
        }
        if (profile) {
            t1 = ag_micros();
            snd_us += (uint64_t)(t1 - t0);
            t0 = t1;
        }

        if (render && scan_line < screen_height) {
            gwenesis_vdp_render_line(scan_line);
        }
        if (profile) {
            t1 = ag_micros();
            vdp_us += (uint64_t)(t1 - t0);
        }

        /* The line-interrupt counter reloads outside the active display. */
        if ((scan_line == 0) || (scan_line > screen_height)) {
            hint_counter = REG10_LINE_COUNTER;
        }
        if (--hint_counter < 0) {
            if ((REG0_LINE_INTERRUPT != 0) && (scan_line <= screen_height)) {
                hint_pending = 1;
                if ((gwenesis_vdp_status & STATUS_VIRQPENDING) == 0) {
                    m68k_update_irq(4);
                }
            }
            hint_counter = REG10_LINE_COUNTER;
        }

        scan_line++;

        /* Vblank starts at the end of the last displayed line. */
        if (scan_line == screen_height) {
            if (REG1_VBLANK_INTERRUPT != 0) {
                gwenesis_vdp_status |= STATUS_VIRQPENDING;
                m68k_set_irq(6);
            }
            if (s_run_z80_cpu) {
                z80_irq_line(1);
            }
        }
        if (scan_line == (screen_height + 1)) {
            if (s_run_z80_cpu) {
                z80_irq_line(0);
            }
        }
    }

    if (s_run_sound) {
        md_sound_end_frame();
    }
    /* Rebase the 68000's counter so next frame starts from zero again. */
    m68k_frame_end(system_clock);
    /* Z80 clock is absolute within the frame; rebase with the 68k. */
    if (zclk >= system_clock) {
        zclk -= system_clock;
    } else {
        zclk = 0;
    }
    if (profile) {
        s_prof_m68k_us = (uint32_t)m68k_us;
        s_prof_z80_us = (uint32_t)z80_us;
        s_prof_snd_us = (uint32_t)snd_us;
        s_prof_vdp_us = (uint32_t)vdp_us;
    }
    frame_counter++;
}

int ag_main(int argc, char **argv)
{
    const char *rom = NULL;
    const char *sound_path = "mock";
    int         frames = -1; /* <0: until Esc/Q */
    int         present_div = 1;
    int         stats = 0;
    int         want_livepad = 1;

    for (int i = 1; i < argc; i++) {
        if (arg_eq(argv[i], "fps30")) {
            present_div = 2;
        } else if (arg_eq(argv[i], "fps60")) {
            present_div = 1;
        } else if (arg_eq(argv[i], "stats")) {
            stats = 1;
        } else if (arg_eq(argv[i], "profile")) {
            /* Per-frame m68k/z80/snd/vdp for the run (use with a frame count). */
            s_profile_frames = 1;
            stats = 1;
        } else if (arg_eq(argv[i], "noz80")) {
            s_run_z80_cpu = 0;
        } else if (arg_eq(argv[i], "nosound")) {
            s_run_sound = 0;
            sound_path = "mock";
        } else if (arg_eq(argv[i], "nolivepad")) {
            want_livepad = 0;
        } else if (arg_eq(argv[i], "livepad")) {
            want_livepad = 1;
        } else if (arg_eq(argv[i], "mock")) {
            sound_path = "mock";
        } else if (arg_eq(argv[i], "wav")) {
            sound_path = "a:\\md.wav";
        } else if (arg_eq(argv[i], "net") || arg_eq(argv[i], "tcp")) {
            sound_path = "net";
        } else if ((argv[i][0] == 'n' || argv[i][0] == 'N') &&
                   (argv[i][1] == 'e' || argv[i][1] == 'E') &&
                   (argv[i][2] == 't' || argv[i][2] == 'T') &&
                   argv[i][3] == ':') {
            sound_path = argv[i]; /* net:PORT */
        } else if (looks_like_number(argv[i])) {
            frames = parse_int(argv[i], -1);
        } else if (rom == NULL) {
            rom = argv[i];
        } else if (argv[i][0] != '\0' &&
                   (argv[i][1] == ':' || argv[i][0] == '/' ||
                    argv[i][0] == '\\' || argv[i][0] == 'a' ||
                    argv[i][0] == 'A' || argv[i][0] == 't' ||
                    argv[i][0] == 'T' || argv[i][0] == 'h' ||
                    argv[i][0] == 'H')) {
            /* Second path-like arg: WAV output (same idea as SMS). */
            sound_path = argv[i];
        }
    }

    md_sound_set_path(sound_path);

    ag_gfxinfo_t info;
    if (ag_gfx_acquire(&info) != AG_OK) {
        ag_printf("md: gfx acquire failed\n");
        return 1;
    }
    ag_gfx_clear(0x00000000u);

    s_ox = ((int)info.width > MD_WIDTH) ? ((int)info.width - MD_WIDTH) / 2 : 0;
    s_oy = ((int)info.height > MD_MAX_LINES)
               ? ((int)info.height - MD_MAX_LINES) / 2
               : 0;
    unsigned short *const origin =
        (unsigned short *)((uint8_t *)info.fb + (size_t)s_oy * info.stride +
                           (size_t)s_ox * sizeof(uint16_t));
    gwenesis_vdp_set_buffer(origin);
    gwenesis_vdp_set_buffer_stride((int)(info.stride / sizeof(uint16_t)));

    size_t         rom_size = 0;
    unsigned char *rom_data = (rom != NULL) ? load_rom_file(rom, &rom_size)
                                            : make_tiny_rom(&rom_size);
    if (rom_data == NULL) {
        ag_gfx_release();
        return 1;
    }

    /* Takes ownership of rom_data and byte-swaps it in place. */
    load_cartridge(rom_data, rom_size);
    power_on();
    reset_emulation();
    md_sound_init();

    s_use_live_pad = want_livepad;
    if (s_use_live_pad) {
        ag_printf("md: controls = live pad (inp / HostFS PADPUSH)\n");
    } else {
        ag_printf("md: controls = serial sticky (no key-up)\n");
    }

    ag_printf("md: %dx%d at %d,%d, rom %u KB, present every %d frame(s)\n",
              MD_WIDTH, MD_MAX_LINES, s_ox, s_oy,
              (unsigned)(rom_size / 1024u), present_div);
    if (!s_run_z80_cpu) {
        ag_printf("md: Z80 CPU off (zclk only; BUSREQ still live)\n");
    }
    if (!s_run_sound) {
        ag_printf("md: sound mix off\n");
    }
    if (s_profile_frames) {
        ag_printf("md: profile = m68k/z80/snd/vdp us per frame\n");
    }

    uint64_t  work_sum = 0;
    uint64_t  emu_sum = 0;
    uint64_t  present_sum = 0;
    uint64_t  work_max = 0;
    uint32_t  window = 0;
    ag_time_t window_t0 = ag_micros();
    int       ran = 0;

    for (; frames < 0 || ran < frames; ran++) {
        const uint32_t  t0 = ag_millis();
        const ag_time_t f0 = ag_micros();

        poll_pad();
        const int show = (present_div <= 1) || ((ran % present_div) == 0);
        if (s_profile_frames && ran >= 120) {
            /* Keep the first two seconds of detail; then normal stats only. */
            s_profile_frames = 0;
        }
        run_frame(show);

        const ag_time_t f1 = ag_micros();
        if (show) {
            ag_gfx_flush((uint16_t)s_ox, (uint16_t)s_oy, MD_WIDTH,
                         (uint16_t)screen_height);
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

        if (s_profile_frames) {
            const unsigned pct =
                (work > 0u) ? (unsigned)(1666700u / (unsigned)work) : 0u;
            ag_printf("md: f%u work %u (m68k %u z80 %u snd %u vdp %u show %u) "
                      "%u%%\n",
                      (unsigned)ran, (unsigned)work, (unsigned)s_prof_m68k_us,
                      (unsigned)s_prof_z80_us, (unsigned)s_prof_snd_us,
                      (unsigned)s_prof_vdp_us, (unsigned)(f2 - f1), pct);
        } else if (stats && (uint64_t)(f2 - window_t0) >= 2000000u) {
            report_stats(window, (uint64_t)(f2 - window_t0), work_sum, emu_sum,
                         present_sum, work_max);
            work_sum = emu_sum = present_sum = work_max = 0;
            window = 0;
            window_t0 = f2;
        }

        ag_yield();
        const uint32_t dt = ag_millis() - t0;
        if (dt < 16u) {
            ag_delay(16u - dt);
        }
    }

    if (window > 0) {
        report_stats(window, (uint64_t)(ag_micros() - window_t0), work_sum,
                     emu_sum, present_sum, work_max);
    }
    ag_printf("md: %d frames\n", ran);
    md_sound_close();
    ag_gfx_release();
    return 0;
}
