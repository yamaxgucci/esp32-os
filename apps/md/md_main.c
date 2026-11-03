/*
 * ArgonOS - Mega Drive player (gwenesis cores), mute milestone.
 *
 *   python tools/mkaxe.py ... -o MD.AXE (see apps/md/README.md)
 *   run a:\MD.AXE a:\game.bin
 *   run a:\MD.AXE 600 stats          # bench the 68000 + VDP path
 *   run a:\MD.AXE a:\game.bin fps30  # emulate 60, show 30
 *   the CYD (MD-NO-PSRAM.AXE, the ROM byte-swapped on the host):
 *   run a:\md-no-psram.axe a:\game.swp waitcard   then Ctrl+\, eject, Alt+1
 *
 * Cores: gwenesis, GPLv3 (see LICENSE and README).  This file: Apache-2.0.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
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
/* a line's horizontal blank, in master clocks: where its picture starts */
#define MD_LINE_HBLANK 860

#ifdef ARGON_MD_SMALL
#include "md_snd_async.h"
#include "md_raster.h"
#endif

/* 24 KB stack: with OpenEth/lwIP the largest internal free block is ~31 KB. */
#ifdef ARGON_MD_SMALL
/*
 * The CYD (ESP32, no PSRAM): the machine's own memory is asked for at start
 * (md_small_alloc), the cartridge is mapped out of flash, and with no display
 * driver loaded it runs without a picture - which is how it is measured.
 * 5 KB of stack asked for: with the Z80 and the chips on this core a whole
 * run, loading included, touched 4.0 KB of it, and they are on the other
 * core now (md_snd_async.c).  The kernel gives a process at least the 8 KB
 * loading needs unless [stacks] load in SYSTEM.CFG says less - the CYD says 5.
 */
#ifdef MD_PSRAM_PAGES
/* a board with PSRAM: the machine's pages that internal memory cannot give
 * come from the process's arena, which is PSRAM there - room for all of them */
AG_APP_SIZED("MD", "0.1", "argon", 0, 5 * 1024, 192 * 1024);
#else
AG_APP_SIZED("MD", "0.1", "argon", 0, 5 * 1024, 4 * 1024);
#endif
#else
AG_APP_SIZED("MD", "0.1", "argon", AG_AXE_NEEDS_GFX, 24 * 1024, 5 * 1024 * 1024);
#endif

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
 * Input: prefer the kernel pad layer (HostFS PADPUSH ??? inp->btnp).  That is
 * real level state, so directions and A+B+C can be held.  `nolivepad` forces
 * the serial sticky path, which has no key-up and is only a degraded reserve.
 */
static int s_use_live_pad = 1;
static int s_run_z80_cpu = 1;   /* 0: advance zclk only, no sms_z80_execute */
static int s_run_sound = 1;
static int s_snd_here;     /* `sndhere`: the sound side stays on this core */     /* 0: skip sample mix (still accepts reg writes) */
static int s_profile_frames = 0; /* m68k/z80/snd/vdp split with the stats */
#ifdef MD_Z80PROF
static int          s_z80bench;
static volatile int s_z80bench_done;
void md_z80_bench(const char *where); /* md_z80.c */
static void md_z80bench_sys(void *arg)
{
    (void)arg;
    md_z80_bench("system core");
    s_z80bench_done = 1;
    ag_thread_exit();
}
#endif
static int s_quit;
/*
 * The front end - the game window, the pause menu, save states - in the
 * small build (the CYD and the S3-CAM): its font is in flash.
 */
#if defined(ARGON_MD_SMALL)
#define MD_MENU 1
#endif
static volatile int s_menu_req; /* Esc seen: the main loop opens the pause menu */
static ag_gfxinfo_t s_gi;

/* run_frame's phases summed over the stats window (us), with `profile`. */
static uint64_t s_prof_m68k_us;
static uint64_t s_prof_z80_us;
static uint64_t s_prof_snd_us;
static uint64_t s_prof_vdp_us;
static uint32_t s_frame_no;     /* frames run so far, for finding the scene */
static uint32_t s_lines_same;   /* lines the glass already showed (md_line_end) */
static uint32_t s_passes;       /* times the cursor went through the glass */
static uint32_t s_draw_us;      /* the drawer in the VDP and the row hash      */
static uint32_t s_gate_us;      /* the drawer waiting for the game's frames    */
static uint32_t s_loop_us;      /* the drawer's passes, start to end           */
static volatile uint32_t s_help_lines; /* lines the application core drew (md_help_draw) */
static uint32_t          s_ras_passes; /* passes drawn from a raster record (the stats) */
static uint32_t s_help_wait_us; /* the system core waiting for one of them     */
static volatile uint32_t s_drawer_stack_left; /* bytes of its stack never used */
static volatile uint32_t s_help_sent;   /* rows the application core sent      */
static int      s_sysdraw = 1;  /* the system core draws the lines (md_drawer) */
static volatile uint32_t s_frames_done; /* frames the emulating core finished */
/* the front end owns the glass: the drawer opens no pass until the next
 * frame, and says it is waiting (s_drawer_parked) */
static volatile int      s_draw_hold, s_drawer_parked;
/* the screen came back from the console: the front end paints again */
static volatile int      s_ui_lost;
static volatile uint32_t s_last_work_us; /* how long the last one took */
#define MD_INFRAME_LINES 28     /* lines a frame may draw while it emulates */
static int      s_inframe_left;
unsigned int md_idle_skips;     /* lines whose rest was a wait (m68kcpu.h) */
#ifdef MD_CYCPROF
unsigned int md_cp_fetch, md_cp_decode, md_cp_exec, md_cp_n, md_cp_mem, md_cp_memn;
#endif

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
#ifdef ARGON_MD_SMALL
static int               s_ring_n;   /* the CYD's picture, below */
static volatile uint32_t s_ring_got;
static volatile uint32_t s_glass_us; /* the system core's time sending rows */
#endif

/* the Z80's share of the system core's sound, ms a second */
static uint32_t md_snd_z80_ms(uint64_t span_us)
{
    extern volatile uint32_t md_snd_z80_us;
    static uint32_t          seen;
    const uint32_t           z = md_snd_z80_us - seen;
    seen += z;
    return (uint32_t)((uint64_t)z * 1000u / span_us);
}

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
    if (s_profile_frames) {
        /* where a frame's time went, and which frame: the scene can be found
         * again by number on the host (scratchpad mdhost, MD_DUMP_EVERY) */
        ag_printf("md: to f%u: m68k %u z80 %u snd %u vdp %u us a frame, "
                  "%u waits skipped\n",
                  (unsigned)s_frame_no,
                  (unsigned)(s_prof_m68k_us / frames),
                  (unsigned)(s_prof_z80_us / frames),
                  (unsigned)(s_prof_snd_us / frames),
                  (unsigned)(s_prof_vdp_us / frames), md_idle_skips);
        md_idle_skips = 0;
        s_prof_m68k_us = s_prof_z80_us = s_prof_snd_us = s_prof_vdp_us = 0;
#ifdef MD_Z80PROF
        { extern void md_z80_prof_report(void); md_z80_prof_report(); }
#endif
#ifdef MD_VDPPROF
        {
            extern unsigned int md_vp_b, md_vp_a, md_vp_s, md_vp_c, md_vp_n;
            extern unsigned int md_vp_cold, md_vp_warm, md_vp_wn;
            if (md_vp_wn) {
                ag_printf("md: a line drawn twice (cycles): first %u, again %u\n",
                          md_vp_cold / md_vp_wn, md_vp_warm / md_vp_wn);
            }
            md_vp_cold = md_vp_warm = md_vp_wn = 0;
            if (md_vp_n) {
                ag_printf("md: a line (cycles): plane B %u, plane A+window %u, "
                          "sprites %u, colours %u; %u lines\n",
                          md_vp_b / md_vp_n, md_vp_a / md_vp_n,
                          md_vp_s / md_vp_n, md_vp_c / md_vp_n, md_vp_n);
            }
            md_vp_b = md_vp_a = md_vp_s = md_vp_c = md_vp_n = 0;
        }
#endif
#ifdef MD_CYCPROF
        if (md_cp_n) {
            ag_printf("md: per instruction (cycles): fetch %u decode %u exec %u "
                      "(memory %u in %u.%u accesses), %u a frame\n",
                      md_cp_fetch / md_cp_n, md_cp_decode / md_cp_n,
                      md_cp_exec / md_cp_n, md_cp_mem / md_cp_n,
                      md_cp_memn / md_cp_n, (md_cp_memn * 10u / md_cp_n) % 10u,
                      md_cp_n / frames);
        }
        md_cp_fetch = md_cp_decode = md_cp_exec = md_cp_n = 0;
        md_cp_mem = md_cp_memn = 0;
#endif
    }
#ifdef ARGON_MD_SMALL
    if (s_ring_n) {
        /* the glass's own rate: rows sent, as whole pictures a second */
        static uint32_t s_rows_seen;
        const uint32_t rows = s_ring_got - s_rows_seen;
        s_rows_seen += rows;
        const unsigned x10 = (unsigned)((uint64_t)rows * 10000000u / span_us /
                                        (uint64_t)(screen_height ? screen_height : 224));
        static uint32_t s_us_seen;
        const uint32_t us = s_glass_us - s_us_seen;
        s_us_seen += us;
        static uint32_t s_passes_seen, s_same_seen;
        const uint32_t passes = s_passes - s_passes_seen;
        const uint32_t same = s_lines_same - s_same_seen;
        s_passes_seen += passes;
        s_same_seen += same;
        const unsigned p10 = (unsigned)((uint64_t)passes * 10000000u / span_us);
        static uint32_t s_draw_seen, s_gate_seen;
        const uint32_t draw = s_draw_us - s_draw_seen, gate = s_gate_us - s_gate_seen;
        s_draw_seen += draw;
        s_gate_seen += gate;
        static uint32_t s_loop_seen;
        const uint32_t loop = s_loop_us - s_loop_seen;
        s_loop_seen += loop;
        static uint32_t s_snd_seen;
        const uint32_t snd = md_snd_core0_us() - s_snd_seen;
        s_snd_seen += snd;
        ag_printf("md: drawer per second: draw %u ms, send %u ms, wait %u ms, "
                  "sound %u ms, z80 %u (%u frames in pieces), passes %u ms\n",
                  (unsigned)((uint64_t)draw * 1000u / span_us),
                  (unsigned)((uint64_t)us * 1000u / span_us),
                  (unsigned)((uint64_t)gate * 1000u / span_us),
                  (unsigned)((uint64_t)snd * 1000u / span_us),
                  (unsigned)md_snd_z80_ms(span_us),
                  (unsigned)md_snd_split(),
                  (unsigned)((uint64_t)loop * 1000u / span_us));
        static uint32_t s_help_seen, s_hwait_seen;
        const uint32_t helped = s_help_lines - s_help_seen;
        const uint32_t hwait = s_help_wait_us - s_hwait_seen;
        s_help_seen += helped;
        s_hwait_seen += hwait;
        static uint32_t s_hsent_seen;
        const uint32_t hsent = s_help_sent - s_hsent_seen;
        s_hsent_seen += hsent;
        ag_printf("md: a ring of %d rows; the application core drew %u lines a "
                  "second and sent %u rows; the system core waited %u ms a second "
                  "for its lines; "
                  "stacks never used: the drawer's %u bytes, this one's %u\n",
                  s_ring_n,
                  (unsigned)((uint64_t)helped * 1000000u / span_us),
                  (unsigned)((uint64_t)hsent * 1000000u / span_us),
                  (unsigned)((uint64_t)hwait * 1000u / span_us),
                  (unsigned)s_drawer_stack_left, (unsigned)ag_stack_left());
        ag_printf("md: glass %u rows, %u.%u pictures a second, %u us a row, "
                  "the system core %u%% busy; %u.%u whole passes a second, "
                  "%u lines unchanged\n", (unsigned)rows,
                  x10 / 10u, x10 % 10u, rows ? (unsigned)(us / rows) : 0u,
                  (unsigned)((uint64_t)us * 100u / span_us),
                  p10 / 10u, p10 % 10u, (unsigned)same);
        if (md_raster_on) {
            extern unsigned md_raster_frames, md_raster_over;
            static unsigned s_rf_seen, s_ro_seen;
            const unsigned rf = md_raster_frames - s_rf_seen;
            const unsigned ro = md_raster_over - s_ro_seen;
            s_rf_seen += rf;
            s_ro_seen += ro;
            static uint32_t s_rp_seen;
            const uint32_t rp = s_ras_passes - s_rp_seen;
            s_rp_seen += rp;
            ag_printf("md: raster record holds %u writes; %u of %u frames had "
                      "more; %u passes drawn from it; scroll table %s\n",
                      md_raster_cap(), ro, rf, (unsigned)rp,
                      md_raster_hs_mode());
        }
    }
#endif
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

#ifdef ARGON_MD_SMALL
extern unsigned char *M68K_RAM;
extern unsigned char *ZRAM;
extern unsigned char *VRAM;
static int            s_headless;
/* the one row a line is drawn into without a ring - only then, from the heap:
 * with a ring both cores draw into the ring (md_send) */
static unsigned short *s_line;

/*
 * The CYD's picture.  The panel has no frame buffer to draw into, and sending
 * a frame down its 40 MHz bus is 50 ms - three frames of the game - so the
 * rows go to the glass from a thread on the system core, through a ring, and
 * the emulation never waits for it: a line is drawn only if the ring has a
 * row free (s_cursor).  The picture is refreshed as fast as the bus allows
 * while the game keeps its own pace.  (The scheme of the CYD's Master System, in rows rather than bands:
 * after the machine's memory there is room for a few rows and a small stack.)
 *
 * Without a thread, or without the memory for a ring, each line is drawn into
 * s_line and sent from here, as the first version did.
 */
#define MD_RING_MAX 16
static unsigned short   *s_ring[MD_RING_MAX];   /* the batch's rows, one block */
static int               s_ring_n;              /* 0: no batch, synchronous */
static int               s_batch_rows;          /* rows drawn into the batch */
static int               s_batch_line;          /* the picture line of its first */
static int               s_batch_w, s_batch_h;  /* screen_width/height, as drawn */
static volatile int      s_batch_full;          /* handed to the thread */
static volatile uint32_t s_ring_got;            /* rows sent */
static volatile int      s_ring_run;
static volatile int      s_ring_go;             /* the machine's memory is there: draw */
static unsigned short   *s_ring_end;            /* one past the ring's block */
static ag_sem_t          s_ring_sem;            /* a batch handed over */
static ag_thread_t       s_presenter;
/*
 * The next line of the picture to draw.  Drawn in order, whenever the batch
 * has room, whatever line the emulation has got to: drawing a line only as
 * the beam passes it left the bus idle while the beam went round to the lines
 * still owed.  A line drawn early or late shows the VDP as it is then - a
 * mid-frame raster trick can tear.
 */
static int               s_cursor;
#define MD_HASH_LINES 240
static uint32_t          s_line_hash[MD_HASH_LINES]; /* what the glass shows */
/* the focus came back: the console has been drawn over the glass, so the
 * next pass clears it and sends every row (md_open_pass) */
static volatile int      s_glass_lost;
static void md_clear_glass(void);
static uint32_t          s_present_us;
static uint32_t          s_present_bad;

/* `rows` consecutive lines from `line`, in consecutive rows of memory. */
static void md_present_rows(const unsigned short *px, int line, int rows, int w,
                            int h)
{
    const ag_blit_t b = {
        .px = px,
        .stride = MD_WIDTH * sizeof(uint16_t),
        .surf_w = s_gi.width,
        .surf_h = s_gi.height,
        .x = (uint16_t)(((int)s_gi.width - w) / 2),
        .y = (uint16_t)(((int)s_gi.height - h) / 2 + line),
        .w = (uint16_t)w,
        .h = (uint16_t)rows,
    };
    if (s_gi.fb != NULL) {
        /*
         * A screen with a framebuffer (the S3-CAM's soft fb0 in front of its
         * ST7789): the rows go into it and that rectangle is flushed - pixels
         * presented past it reached the panel and were painted over by the
         * framebuffer's own refresh, and the glass showed nothing of the game
         * (1 Nov 2026).  The CYD's screen has no framebuffer and takes rows.
         */
        for (int r = 0; r < rows; r++) {
            memcpy((uint8_t *)s_gi.fb + (size_t)(b.y + r) * s_gi.stride +
                       (size_t)b.x * sizeof(uint16_t),
                   px + (size_t)r * MD_WIDTH, (size_t)w * sizeof(uint16_t));
        }
        ag_gfx_flush(b.x, b.y, (uint32_t)w, (uint32_t)rows);
        return;
    }
    if (ag_gfx_present(&b) != AG_OK) {
        s_present_bad++;
    }
}

/*
 * The system core's half: a whole batch to the glass in one call.  A call
 * costs some 300 us here whatever it carries and a row some 30 more, so the
 * rows go in batches; and the thread is woken by the batch, not by the tick -
 * sleeping a tick let four rows out per three milliseconds.
 */
/* A drawn row against what the glass last got for that line. */
MD_HOT_VDP static uint32_t md_row_hash(const unsigned short *row, int width)
{
    const uint32_t *w = (const uint32_t *)(const void *)row;
    uint32_t h = 2166136261u ^ (uint32_t)width;
    for (int i = 0; i < width / 2; i++) {
        h = (h ^ w[i]) * 16777619u;
    }
    return h;
}

/*
 * The system core draws the picture as well as sending it (`sysdraw`, the
 * default with a panel): line after line, top to bottom, as fast as the VDP
 * and the wire allow, whatever the emulation is doing.
 *
 * Drawn by the emulating core, the lines came out at the pace that core could
 * spare - 28 a frame in a busy scene - so one pass over the glass took eight
 * frames and he could see the picture being redrawn in bands.  Here a pass is
 * as long as drawing and sending its changed lines take, and the emulating
 * core does no drawing at all.  The VDP's state is read while the other core
 * changes it, which shows as what it always did: a line from a frame or so
 * earlier or later than its neighbour, never a fault.
 *
 * Each pass ends with a millisecond's sleep, and a pass that changed nothing
 * with four: this core also runs the system's own tasks, and a thread that
 * never sleeps starves them.
 */
/*
 * Both cores draw the picture, and whichever is free sends it.
 *
 * The system core alone came to some 25 whole pictures a second of Sonic,
 * with the sound on its hands as well: drawing ~490 ms of its second, sending
 * ~140, the sound ~320 (30 Oct 2026) - while the application core slept a
 * third of its own second between frames.  Handing that core's lines over one
 * at a time did not help: its spare time comes when its frame ends, which is
 * when the system core takes the frame's sound - 5 ms in one piece - and each
 * line it drew waited for that.  It drew 450 lines a second.
 *
 * So the ring is shared.  Line L of a pass is drawn into row L % N, by the
 * core that claims it (s_next, one counter, so each line is drawn once), and
 * only when that row is free - when line L - N has gone (s_sent).  Rows go to
 * the glass strictly in order, by whichever core holds the sender's token
 * (md_send): the panel's window just runs on, as it did with one sender.
 * The application core, ahead of the system core's sound, draws and sends on
 * its own.
 *
 * A row's line is compared with what the glass last got for that line
 * (s_line_hash) when it is sent: an unchanged line is passed over.
 */
/*
 * The words both cores change with an atomic instruction, apart from the rest:
 * on the S3-CAM an application's data is in PSRAM, where the S3 has no atomic
 * instructions - the sending flag stayed taken with nobody sending, and the
 * picture stopped a few seconds in for good (1 Nov 2026).  There they are
 * moved to internal memory (md_sync_internal).
 */
static uint32_t   s_sync_words[3];
uint32_t         *md_sync = s_sync_words; /* md_raster.c takes its lock here */
#define s_next   (((volatile uint32_t *)md_sync)[0]) /* the pass's next line to draw */
#define s_sender (((volatile uint32_t *)md_sync)[1]) /* 1 while a core sends */
#define MD_SYNC_WORDS (sizeof(s_sync_words) / sizeof(s_sync_words[0]))

static void md_sync_internal(void)
{
    if (md_sync != s_sync_words) {
        return; /* moved already */
    }
#ifdef MD_PSRAM_PAGES
    uint32_t *const p = (uint32_t *)ag_malloc_caps(sizeof(s_sync_words), AG_MEM_FAST);
    if (p != NULL) {
        for (unsigned i = 0; i < MD_SYNC_WORDS; i++) {
            p[i] = 0;
        }
        md_sync = p;
    }
#endif
}
static volatile int      s_sent;     /* lines before this one are dealt with    */
static volatile int      s_pass_h;   /* the pass's lines while one is open, else 0 */
static volatile uint8_t  s_row_ready[MD_RING_MAX];
static volatile uint32_t s_row_hash[MD_RING_MAX];
static int               s_help = 1; /* `nohelp`: the system core alone         */
/* the pass draws from the last frame's raster record (md_raster.c): a
 * line with writes before it (s_ras_gate) is claimed only once every line
 * above it is drawn, and its claimer puts the writes in */
static volatile int      s_pass_raster;
static volatile int      s_ras_gate;
/*
 * `shotN`: the first pass after frame N printed as it is drawn - every line,
 * "S<line> <x> <pixels in hex>" - for the host to put together into what
 * went to the glass (scratchpad shot.py).  One core draws it, in order.
 */
static uint32_t          s_shot_at;
static volatile int      s_shot_now;

static int md_core(void)
{
    unsigned int id;
    __asm__ volatile("rsr %0, prid" : "=a"(id));
    return (int)((id >> 13) & 1u);
}

/*
 * Every line above `line` drawn: those not sent yet are at most a ring's
 * rows, each ready once drawn.  A row sent meanwhile reads as not ready, and
 * the caller asks again.  Asked only at a raster pass's gate: a count of
 * lines drawn, kept with an atomic add at every line by both cores, cost
 * Sonic Spinball 42 -> 36 pictures a second (31 Oct 2026).
 */
static bool drawn_above(int line)
{
    const int n = s_ring_n;
    for (int l = s_sent; l < line; l++) {
        if (!s_row_ready[l % n]) {
            return false;
        }
    }
    return true;
}

/* The pass's next line, if its row is free; -1 if not, or no pass is open. */
static int claim_line(void)
{
    uint32_t c = s_next;
    for (;;) {
        if ((int)c >= s_pass_h || (int)c >= s_sent + s_ring_n) {
            return -1;
        }
        const int gate = s_ras_gate;
        if (s_pass_raster && (int)c >= gate && ((int)c > gate || !drawn_above(c))) {
            return -1;        /* the lines above are still being drawn */
        }
        if (__atomic_compare_exchange_n((uint32_t *)&s_next, &c, c + 1u, false,
                                        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
            if (s_pass_raster && (int)c == gate) {
                md_raster_line((int)c);
                __sync_synchronize();
                s_ras_gate = md_raster_next();
            }
            return (int)c;
        }
    }
}

static void draw_line(int line)
{
    const int slot = line % s_ring_n;
    gwenesis_vdp_set_buffer(s_ring[slot]);
#ifdef MD_VDPPROF
    if ((line & 31) == 7) {
        /* the same line twice: the second with the code just run */
        extern unsigned int md_vp_cold, md_vp_warm, md_vp_wn;
        unsigned int c0, c1, c2;
        __asm__ volatile("rsr %0, ccount" : "=a"(c0));
        gwenesis_vdp_render_line(line);
        __asm__ volatile("rsr %0, ccount" : "=a"(c1));
        gwenesis_vdp_render_line(line);
        __asm__ volatile("rsr %0, ccount" : "=a"(c2));
        md_vp_cold += c1 - c0;
        md_vp_warm += c2 - c1;
        md_vp_wn++;
    } else
#endif
    gwenesis_vdp_render_line(line);
    s_row_hash[slot] = md_row_hash(s_ring[slot], screen_width);
    if (s_shot_now) {
        const uint16_t *px = s_ring[slot];
        for (int x = 0; x < screen_width; x += 16) {
            char hex[16 * 4 + 1]; /* small: the drawer's stack has ~500 bytes spare */
            for (int k = 0; k < 16; k++) {
                static const char d[] = "0123456789abcdef";
                const uint16_t v = px[x + k];
                hex[k * 4] = d[v >> 12];
                hex[k * 4 + 1] = d[(v >> 8) & 15];
                hex[k * 4 + 2] = d[(v >> 4) & 15];
                hex[k * 4 + 3] = d[v & 15];
            }
            hex[16 * 4] = 0;
            ag_printf("S%d %d %s\n", line, x, hex);
            /* the console is a ring smaller than a line: each 72 bytes
             * reach the UART (11.5 KB/s) before the next are written */
            ag_delay(8);
        }
    }
    __sync_synchronize();
    s_row_ready[slot] = 1;
}

/*
 * What is ready, in order, to the glass: rows a call, as many as are ready in
 * a run and changed, up to the end of the ring.  A run shorter than four that
 * could still grow - its next row not drawn yet - waits for it: a call costs
 * about the same whatever it carries.  True if anything went.
 */
static bool md_send(void)
{
    if (__atomic_exchange_n((uint32_t *)&s_sender, 1u, __ATOMIC_ACQUIRE) != 0u) {
        return false;
    }
    const int  n = s_ring_n;
    const int  w = screen_width, h = screen_height;
    const bool here = (md_core() == 0);
    bool       went = false;
    for (;;) {
        const int line = s_sent;
        const int hh = s_pass_h;
        if (line >= hh) {
            break;
        }
        const int slot = line % n;
        if (!s_row_ready[slot]) {
            break;
        }
        __sync_synchronize();
        if (s_row_hash[slot] == s_line_hash[line]) {
            s_lines_same++;   /* the glass has it */
            s_row_ready[slot] = 0;
            __sync_synchronize();
            s_sent = line + 1;
            went = true;
            continue;
        }
        int k = 1;
        while (slot + k < n && line + k < hh && s_row_ready[slot + k] &&
               s_row_hash[slot + k] != s_line_hash[line + k]) {
            k++;
        }
        if (k < 4 && slot + k < n && line + k < hh && !s_row_ready[slot + k]) {
            break;            /* its next row is still to come: wait for it */
        }
        for (int i = 0; i < k; i++) {
            s_line_hash[line + i] = s_row_hash[slot + i];
        }
        const ag_time_t t0 = ag_micros();
        md_present_rows(s_ring[slot], line, k, w, h);
        if (here) {
            s_glass_us += (uint32_t)(ag_micros() - t0);
        } else {
            s_help_sent += (uint32_t)k;
        }
        s_ring_got += (uint32_t)k;
        for (int i = 0; i < k; i++) {
            s_row_ready[slot + i] = 0;
        }
        __sync_synchronize();
        s_sent = line + k;
        went = true;
    }
    __atomic_store_n((uint32_t *)&s_sender, 0u, __ATOMIC_RELEASE);
    return went;
}

/* The application core: lines, and rows to the glass, until `until`. */
static void md_help_draw(ag_time_t until)
{
    while ((int64_t)(until - ag_micros()) > 250) {
        if (s_pass_h == 0 || MODE_SHI || s_shot_now) {
            return;   /* between passes; shadow/highlight has one sprite buffer */
        }
        if (md_send()) {
            continue;
        }
        const int line = claim_line();
        if (line >= 0) {
            draw_line(line);
            s_help_lines++;
        }
    }
}

static void md_open_pass(void)
{
    const int h = (screen_height < MD_HASH_LINES) ? screen_height : MD_HASH_LINES;
    if (s_glass_lost) {
        /* between passes nothing is in the ring: its first row is free */
        s_glass_lost = 0;
        md_clear_glass();
        memset(s_line_hash, 0xff, sizeof(s_line_hash));
    }
    int took = md_raster_on ? md_raster_take() : 0;
    while (took == 2 && s_ring_run) {
        /* the frame it would draw went by while the last pass held the
         * copy: the next one, not the same one again */
        if (!md_snd_core0_poll()) {
            ag_delay(1);
        }
        took = md_raster_take();
    }
    s_pass_raster = (took == 1);
    s_ras_gate = s_pass_raster ? md_raster_next() : 0x7fffffff;
    s_ras_passes += s_pass_raster ? 1u : 0u;
    if (s_shot_at != 0u && s_frames_done >= s_shot_at) {
        s_shot_at = 0;
        s_shot_now = 1;
        ag_printf("SHOT frame %u raster %d width %d\n", (unsigned)s_frames_done,
                  s_pass_raster, screen_width);
        ag_delay(50);
    }
    s_next = 0;
    s_sent = 0;
    __sync_synchronize();
    s_pass_h = h;
}

static void md_drawer(void)
{
    uint32_t pass_seen = s_frames_done;   /* frames done when this pass began */
    ag_time_t pass_t0 = ag_micros();
    for (int i = 0; i < MD_RING_MAX; i++) {
        s_row_ready[i] = 0;
    }
    md_open_pass();
    while (s_ring_run) {
        /* the sound side first: a frame's log played before the next row -
         * a late picture is a band, late sound is a gap */
        (void)md_snd_core0_poll();
        (void)md_send();
        const int line = claim_line();
        if (line >= 0) {
            const ag_time_t d0 = ag_micros();
            draw_line(line);
            s_draw_us += (uint32_t)(ag_micros() - d0);
            continue;
        }
        if (s_sent < s_pass_h) {
            /* the other core is drawing the line the glass needs next */
            const ag_time_t t0 = ag_micros();
            while (s_ring_run && s_sent < s_pass_h && !md_send() &&
                   !((int)s_next < s_pass_h && (int)s_next < s_sent + s_ring_n)) {
                /*
                 * The sound too, while waiting: the 68000 waits at its next
                 * line for this core to have played the last frame's sound,
                 * and the line waited for here may be one it is to draw.
                 */
                (void)md_snd_core0_poll();
            }
            s_help_wait_us += (uint32_t)(ag_micros() - t0);
            continue;
        }
        s_pass_h = 0;             /* closed: the other core stops */
        s_passes++;
        if (s_pass_raster) {
            s_pass_raster = 0;
            md_raster_done();
        }
        if (s_shot_now) {
            s_shot_now = 0;
            ag_printf("SHOT end\n");
        }
        /*
         * The next pass starts with the next frame, never sooner.  A pass
         * more often than the game makes pictures draws the same picture
         * again, and drawing it is flash traffic both cores queue for: 34
         * passes a second over a game making 10 frames a second took the
         * 68000 from 30 ms a frame to 95 (29 Oct 2026).  Starting just
         * after a frame ends also starts it on one whole picture.
         *
         * Counted from when the pass BEGAN, not from when it ended: a pass
         * over a busy scene is itself two or three frames long, and
         * waiting the frames again after it held Sonic to 9 passes a
         * second whether the wire ran at 40 MHz or 80 (30 Oct 2026).
         */
        {
            /* and one frame later than that while the game is behind:
             * the drawing is what it is queueing behind */
            const uint32_t wait = (s_last_work_us > 16683u) ? 2u : 1u;
            const ag_time_t g0 = ag_micros();
            ag_delay(1);             /* the system core's own tasks */
            s_drawer_parked = 1;
            while (s_ring_run && (s_draw_hold || s_frames_done - pass_seen < wait)) {
                if (!md_snd_core0_poll()) {
                    ag_delay(1);
                }
            }
            s_drawer_parked = 0;
            pass_seen = s_frames_done;
            const ag_time_t g1 = ag_micros();
            s_gate_us += (uint32_t)(g1 - g0);
            s_loop_us += (uint32_t)(g1 - pass_t0);
            pass_t0 = g1;
        }
        s_drawer_stack_left = ag_stack_left();
        md_open_pass();
    }
}

static void md_presenter(void *arg)
{
    (void)arg;
    if (s_sysdraw) {
        while (s_ring_run && (s_ring_n == 0 || !s_ring_go)) {
            ag_delay(5);             /* the rows arrive just after the thread */
        }
        md_drawer();
        ag_thread_exit();
        return;
    }
    while (s_ring_run) {
        (void)ag_sem_take(s_ring_sem, 50);
        if (!s_batch_full) {
            continue;
        }
        __sync_synchronize();
        const ag_time_t t0 = ag_micros();
        md_present_rows(s_ring[0], s_batch_line, s_batch_rows, s_batch_w,
                        s_batch_h);
        s_glass_us += (uint32_t)(ag_micros() - t0);
        s_ring_got += (uint32_t)s_batch_rows;
        s_batch_rows = 0;
        __sync_synchronize();
        s_batch_full = 0;
    }
    ag_thread_exit();
}

/*
 * The batch and its thread, taken before the machine's memory: they need whole
 * blocks, which the machine's pages - cut out of anything - leave none of.
 */
static void md_ring_start(int rows)
{
    if (s_ring_n > 0 && s_ring_sem != NULL) {
        /* the ring kept from the last game: only its thread again */
        s_ring_run = 1;
        s_presenter = ag_thread_create(md_presenter, NULL, "mdglass", 2560, 4,
                                       AG_THREAD_SYS_CORE);
        if (s_presenter == NULL) {
            s_ring_run = 0;
        }
        return;
    }
    s_ring_sem = ag_sem_create(0, 4);
    if (s_ring_sem == NULL) {
        return;
    }
    s_ring_run = 1;
    /* 2.5 KB: Sonic with the sound left 2 KB of 4 never used, and an H32
     * line no longer takes 1152 bytes of it (gwenesis_vdp_gfx.c); at 3 KB
     * 1044 bytes were never used (31 Oct 2026) */
    s_presenter = ag_thread_create(md_presenter, NULL, "mdglass", 2560, 4,
                                   AG_THREAD_SYS_CORE);
    if (s_presenter == NULL) {
        s_ring_run = 0;
        return;
    }
    for (int n = rows; n >= 2; n--) {
        const size_t bytes = (size_t)n * MD_WIDTH * sizeof(uint16_t);
        unsigned short *b = (unsigned short *)ag_malloc_caps(bytes, AG_MEM_FAST);
        if (b == NULL) {
            b = (unsigned short *)ag_malloc(bytes);
        }
        if (b != NULL) {
            for (int i = 0; i < n; i++) {
                s_ring[i] = b + (size_t)i * MD_WIDTH;
            }
            s_ring_end = b + (size_t)n * MD_WIDTH;
            __sync_synchronize();
            s_ring_n = n;
            return;
        }
    }
    /* no batch after all: the thread idles until the end */
}

static void md_ring_stop(void)
{
    if (s_presenter != NULL) {
        s_ring_run = 0;
        ag_sem_give(s_ring_sem);
        (void)ag_thread_join(s_presenter, 1000);
        s_presenter = NULL;
    }
}

/* Black over the rows the picture does not cover, once: the console's text. */
static int md_line_alloc(void)
{
    if (s_line == NULL) {
        s_line = (unsigned short *)ag_malloc(MD_WIDTH * sizeof(uint16_t));
        if (s_line == NULL) {
            ag_printf("md: no memory for a line\n");
            return 0;
        }
    }
    return 1;
}

static void md_clear_glass(void)
{
    if (s_gi.fb != NULL) {
        /* a screen with a framebuffer (the S3-CAM): black into it - pixels
         * presented past it are painted over by its own refresh, and the
         * game window's rows above and below the picture stayed (3 Nov 2026) */
        for (uint32_t y = 0; y < s_gi.height; y++) {
            memset((uint8_t *)s_gi.fb + (size_t)y * s_gi.stride, 0,
                   (size_t)s_gi.width * sizeof(uint16_t));
        }
        ag_gfx_flush(0, 0, s_gi.width, s_gi.height);
        return;
    }
    /*
     * One black row, sent down the glass a row at a time.  It used to go as
     * one call with a stride of zero, and gfx_present refuses a stride under
     * the row's width - the glass was never cleared, and the game window's
     * rows stayed above and below every game (3 Nov 2026).
     */
    unsigned short *const row = (s_ring_n > 0) ? s_ring[0] : s_line;
    const uint16_t w = (s_gi.width < MD_WIDTH) ? (uint16_t)s_gi.width : (uint16_t)MD_WIDTH;
    memset(row, 0, MD_WIDTH * sizeof(uint16_t));
    for (uint16_t y = 0; y < s_gi.height; y++) {
        const ag_blit_t b = {
            .px = row, .stride = (uint32_t)w * sizeof(uint16_t),
            .surf_w = s_gi.width, .surf_h = s_gi.height,
            .x = 0, .y = y, .w = w, .h = 1,
        };
        (void)ag_gfx_present(&b);
    }
}

/*
 * At each line of the emulation: which line of the picture to draw now, if
 * any, with the VDP pointed at where it goes.  Without the batch it is the
 * line the emulation is on; with it, the next the picture needs, while the
 * batch is not on its way to the glass.
 */
static int md_line_begin(int scan)
{
    if (s_headless || s_ring_n == 0) {
        if (scan >= screen_height) {
            return -1;
        }
        gwenesis_vdp_set_buffer(s_line);
        return scan;
    }
    if (s_batch_full) {
        return -1;
    }
    /*
     * Inside a frame (any scan at all is the emulation asking; the
     * spare-time loop asks with -1) only a few
     * lines: drawing one costs the VDP ~50 us whether or not it turns out
     * unchanged, and every line of every frame is 11 ms of a 16.7 ms frame.
     * The time after the frame draws the rest.
     */
    if (scan >= 0) {
        if (s_inframe_left <= 0) {
            return -1;
        }
        s_inframe_left--;
    }
    if (s_cursor >= screen_height) {
        s_cursor = 0;
        s_passes++;           /* the whole glass brought up to date once more */
    }
    gwenesis_vdp_set_buffer(s_ring[s_batch_rows]);
    return s_cursor;
}

/* After: the row goes to the glass - with its batch, or sent from here. */
static void md_line_end(int line)
{
    if (s_headless) {
        return;
    }
    if (s_ring_n == 0) {
        const ag_time_t t0 = ag_micros();
        md_present_rows(s_line, line, 1, screen_width, screen_height);
        s_present_us += (uint32_t)(ag_micros() - t0);
        return;
    }
    /*
     * A line the glass already shows is not sent again.  The wire is the
     * limit - a row is 640 bytes, 128 us at 40 MHz - and on most screens
     * much of the picture stands still: the sky, a border, a score.  A batch
     * holds consecutive lines, so a skipped line sends what was gathered.
     */
    {
        const uint32_t *w = (const uint32_t *)(const void *)s_ring[s_batch_rows];
        uint32_t h = 2166136261u ^ (uint32_t)screen_width;
        for (int i = 0; i < screen_width / 2; i++) {
            h = (h ^ w[i]) * 16777619u;
        }
        if (line < MD_HASH_LINES && s_line_hash[line] == h) {
            s_lines_same++;
            s_cursor = line + 1;
            if (s_batch_rows > 0) {
                __sync_synchronize();
                s_batch_full = 1;
                ag_sem_give(s_ring_sem);
            }
            return;
        }
        if (line < MD_HASH_LINES) {
            s_line_hash[line] = h;
        }
    }
    if (s_batch_rows == 0) {
        s_batch_line = line;
        s_batch_w = screen_width;
        s_batch_h = screen_height;
    }
    s_batch_rows++;
    s_cursor = line + 1;
    if (s_batch_rows == s_ring_n || s_cursor >= screen_height) {
        __sync_synchronize();
        s_batch_full = 1;
        ag_sem_give(s_ring_sem);
        /* the thread empties the batch (s_batch_rows) before it lets go */
    }
}

static int s_blocks; /* blocks the pages were cut from */

/* internal memory for small things, after the pages: what a page cannot use */
static void *md_crumb_alloc(size_t n)
{
    void *b = ag_malloc_caps(n, AG_MEM_FAST);
#ifdef MD_PSRAM_PAGES
    /* a record in PSRAM once stopped the 68000's loop on the S3-CAM: its
     * lock was in PSRAM too, where the S3 has no atomics (md_sync) */
    if (b == NULL) {
        b = ag_malloc_caps(n, AG_MEM_DEFAULT);
    }
#endif
    return b;
}

/*
 * Pages out of heap blocks of `caps`, each block as large as the heap will
 * give for what is still wanted, none under `least`: every block is one of
 * the process's 32 resources, and blocks of fixed sizes from 32 KB down took
 * 24 of them on the CYD - the raster record, which lives in the holes the
 * pages leave, then found no resource left for itself (1 Nov 2026).
 */
/* the blocks the pages came from, and the ring as it was, for md_pages_free */
static void           *s_page_blk[48];
static int             s_page_blk_n;
static int             s_ring_n0;
static unsigned short *s_ring_end0;

static void page_block(void *b)
{
    if (s_page_blk_n < (int)(sizeof(s_page_blk) / sizeof(s_page_blk[0]))) {
        s_page_blk[s_page_blk_n++] = b;
    }
}

static unsigned heap_pages(unsigned char **pages, unsigned got, unsigned count,
                           unsigned caps, size_t least)
{
    if (least < MD_PAGE) {
        least = MD_PAGE;
    }
    size_t size = (size_t)(count - got) * MD_PAGE;
    while (got < count && size >= least) {
#ifdef MD_PSRAM_PAGES
        if (caps == AG_MEM_FAST) {
            /*
             * Internal memory is the system's on a board with PSRAM for the
             * rest: with the pages in it the S3-CAM had 14 KB left, and the
             * phone's driver turned the pad away on a reconnect (its floor is
             * 16 KB) - the game window after LOAD GAME took no presses
             * (3 Nov 2026).
             */
            ag_meminfo_t mi;
            ag_meminfo(&mi);
            const size_t keep = 48u * 1024u;
            const size_t room = (mi.fast_free > keep) ? mi.fast_free - keep : 0u;
            if (size > room) {
                size = (room / MD_PAGE) * MD_PAGE;
                if (size < least) {
                    break;
                }
            }
        }
#endif
        unsigned char *b = (unsigned char *)ag_malloc_caps(size, caps);
        if (b == NULL) {
            size -= MD_PAGE;
            continue;
        }
        page_block(b);
        s_blocks++;
        for (size_t o = 0; o < size; o += MD_PAGE) {
            pages[got++] = b + o;
        }
        if (size > (size_t)(count - got) * MD_PAGE) {
            size = (size_t)(count - got) * MD_PAGE;
        }
    }
    return got;
}

/*
 * Fill `pages[0..count)` from the largest blocks the heap gives, a page at a
 * time cut out of each: what is left after the image is in pieces, and every
 * system-heap block is one of the process's 32 resources, so asking page by
 * page would run out of either.  `iram` asks for IRAM, which only VRAM can
 * use; otherwise internal byte memory, then the process's own arena.
 */
static unsigned fill_pages(unsigned char **pages, unsigned count, int iram)
{
    const unsigned caps = iram ? AG_MEM_EXEC : AG_MEM_FAST;
    /*
     * Blocks under 4 KB last, after the arena: each costs a resource as a
     * large one does.
     */
    unsigned got = heap_pages(pages, 0, count, caps, 4096);
#ifdef MD_PSRAM_PAGES
    /*
     * A board with PSRAM (the S3-CAM): what internal memory could not give,
     * from PSRAM, in blocks as large as it has - the machine's RAM is read
     * through the data cache there, and internal memory on that board is
     * what the radio and the system keep (1 Nov 2026: 34 of 64 KB of the
     * 68000's RAM came from it, and MD stopped short).
     */
    if (!iram) {
        got = heap_pages(pages, got, count, AG_MEM_DEFAULT, 0);
    }
#endif
    /*
     * Then the process's own arena, by what its largest block actually holds
     * rather than by powers of two: its allocator keeps its bookkeeping inside
     * it, so a 16 KB arena never has a 16 KB block, and halving from there
     * leaves kilobytes behind.
     */
    while (!iram && got < count) {
        ag_meminfo_t mi;
        ag_meminfo(&mi);
        size_t size = (mi.arena_largest / MD_PAGE) * MD_PAGE;
        if (size > (size_t)(count - got) * MD_PAGE) {
            size = (size_t)(count - got) * MD_PAGE;
        }
        unsigned char *b = NULL;
        while (size >= MD_PAGE && (b = (unsigned char *)ag_malloc(size)) == NULL) {
            size -= MD_PAGE;
        }
        if (b == NULL) {
            break;
        }
        page_block(b);
        for (size_t o = 0; o < size; o += MD_PAGE) {
            pages[got++] = b + o;
        }
    }
    return heap_pages(pages, got, count, caps, 0);
}

/*
 * The machine's memory, in pages (md_pages.h).  VRAM first into IRAM,
 * which takes only word accesses - the VDP makes no other kind (VRAM_W in
 * gwenesis_vdp.h) - and which nothing else here could use; the VRAM pages that
 * do not fit there come from the heap last.
 */
/*
 * Pages out of the ring's last rows, the ring keeping two: a 640-byte row
 * holds a 512-byte page, four rows five.  A smaller ring draws less ahead and
 * sends fewer rows a call - a slower picture, not a game that will not start.  The machine's memory comes in
 * pages cut out of whatever the heap has left, in pieces, and whether the last
 * page is there depended on how the heap lay - Sonic started and Alex Kidd,
 * the same build a minute later, was one page of VRAM short with 4 KB free in
 * pieces too small (30 Oct 2026).
 */
static unsigned ring_pages(unsigned char **out, unsigned want)
{
    unsigned got = 0;
    while (got < want && s_ring_n > 2) {
        s_ring_n--;
        /* pages off the top: what is left of a row joins the one below */
        unsigned char *const floor = (unsigned char *)s_ring[s_ring_n];
        unsigned char *end = (unsigned char *)s_ring_end;
        while (got < want && end - floor >= (ptrdiff_t)MD_PAGE) {
            end -= MD_PAGE;
            out[got++] = end;
        }
        s_ring_end = (unsigned short *)(void *)end;
    }
    return got;
}

/* the machine's memory given back, the ring's rows with it */
static void md_pages_free(void)
{
    for (int i = 0; i < s_page_blk_n; i++) {
        ag_free(s_page_blk[i]);
    }
    s_page_blk_n = 0;
    s_blocks = 0;
    if (s_ring_n0 > 0) {
        s_ring_n = s_ring_n0;
        s_ring_end = s_ring_end0;
    }
}

static int md_small_alloc(void)
{
    s_ring_n0 = s_ring_n;
    s_ring_end0 = s_ring_end;
    const unsigned in_iram = fill_pages(md_vram, MD_VRAM_PAGES, 1);
    unsigned ram = fill_pages(md_ram, MD_RAM_PAGES, 0);
    /*
     * The Z80's first page is the image's own: allocated with it at load,
     * before the heap is in pieces - by now what is left may not hold one more
     * block.
     */
    static unsigned char s_zram0[MD_PAGE] __attribute__((aligned(4)));
    unsigned zram = 1;
    md_zram[0] = s_zram0;
    if (!s_run_z80_cpu) {
        /*
         * `noz80`: nothing runs the Z80's 8 KB, so one page stands for all of
         * them - the 68000 may still write its sound program there and read
         * a byte back, and gets a byte.  7 KB the picture needs more.
         */
        for (unsigned i = 1; i < MD_ZRAM_PAGES; i++) {
            md_zram[i] = md_zram[0];
        }
        zram = MD_ZRAM_PAGES;
    } else {
        zram += fill_pages(md_zram + 1, MD_ZRAM_PAGES - 1u, 0);
    }
    unsigned vram = in_iram + fill_pages(md_vram + in_iram, MD_VRAM_PAGES - in_iram, 0);
    /* what the heap could not give, out of the ring */
    ram += ring_pages(md_ram + ram, MD_RAM_PAGES - ram);
    zram += ring_pages(md_zram + zram, MD_ZRAM_PAGES - zram);
    vram += ring_pages(md_vram + vram, MD_VRAM_PAGES - vram);
    if (ram < MD_RAM_PAGES || zram < MD_ZRAM_PAGES || vram < MD_VRAM_PAGES) {
        ag_meminfo_t mi;
        ag_meminfo(&mi);
#define MD_PAGES_KB(n) ((unsigned)((n) * MD_PAGE / 1024u))
        ag_printf("md: short of memory - 68000 RAM %u/64 KB, Z80 RAM %u/8 KB, "
                  "VRAM %u/64 KB (%u in IRAM), %d blocks; internal %u free\n",
                  MD_PAGES_KB(ram), MD_PAGES_KB(zram), MD_PAGES_KB(vram),
                  MD_PAGES_KB(in_iram), s_blocks, (unsigned)mi.fast_free);
        return 0;
    }
    M68K_RAM = md_ram[MD_RAM_PAGES - 1];
    ZRAM = md_zram[MD_ZRAM_PAGES - 1];
    VRAM = md_vram[0];
    ag_printf("md: 68000 RAM 64 KB, Z80 RAM %s, VRAM 64 KB (%u KB in IRAM) - "
              "%u-byte pages from %d blocks; %d rows of ring\n",
              s_run_z80_cpu ? "8 KB" : "one page (no Z80)", MD_PAGES_KB(in_iram),
              (unsigned)MD_PAGE, s_blocks, s_ring_n);
    __sync_synchronize();
    s_ring_go = 1;
    return 1;
}

/* Read in place out of flash; the host swapped its bytes (load_cartridge). */
static unsigned char *map_rom(const char *path, size_t *size_out)
{
    const void *ptr = NULL;
    uint64_t    len = 0;
    const ag_time_t t0 = ag_micros();
    const ag_err_t err = ag_map(path, &ptr, &len);
    if (err != AG_OK) {
        ag_printf("md: %s: %s\n", path, ag_strerror(err));
        return NULL;
    }
    ag_printf("md: %s, %u KB, staged in flash in %u ms\n", path,
              (unsigned)(len / 1024u), (unsigned)((ag_micros() - t0) / 1000u));
    *size_out = (size_t)len;
    return (unsigned char *)ptr;
}
#endif

static void bind_vdp_to_fb(const ag_gfxinfo_t *info)
{
    s_ox = ((int)info->width > MD_WIDTH) ? ((int)info->width - MD_WIDTH) / 2 : 0;
    s_oy = ((int)info->height > MD_MAX_LINES)
               ? ((int)info->height - MD_MAX_LINES) / 2
               : 0;
    unsigned short *const origin =
        (unsigned short *)((uint8_t *)info->fb + (size_t)s_oy * info->stride +
                           (size_t)s_ox * sizeof(uint16_t));
    gwenesis_vdp_set_buffer(origin);
    gwenesis_vdp_set_buffer_stride((int)(info->stride / sizeof(uint16_t)));
}

static int handle_session_ev(const ag_event_t *ev)
{
    if (ev->type == AG_EV_FOCUS_GAINED) {
#ifdef ARGON_MD_SMALL
        /*
         * The CYD's picture goes out as rows (md_present_rows), never through
         * a framebuffer: a surfaceless screen has none, and binding the VDP
         * to its address put the next line at an offset from NULL - the
         * Mega Drive wrote over a task's control block and both cores went
         * down, once the kernel began answering an owner's re-acquire with
         * yes instead of busy (1 Nov 2026).  So: the screen, and every row
         * sent again, since the console was drawn over them.
         */
        if (!s_headless && ag_gfx_acquire(&s_gi) == AG_OK) {
            s_ui_lost = 1;
            if (s_ring_n > 0) {
                s_glass_lost = 1; /* the drawer's, between its passes */
            } else {
                md_clear_glass();
                memset(s_line_hash, 0xff, sizeof(s_line_hash));
            }
        }
        return 1;
#endif
        if (ag_gfx_acquire(&s_gi) == AG_OK) {
            /*
             * acquire() copies the previous front (often FM/console) into the
             * back buffer; MD only flushes its 320xN window.  Wipe letterbox.
             */
            ag_gfx_clear(0x00000000u);
            bind_vdp_to_fb(&s_gi);
            ag_gfx_flush(0, 0, s_gi.width, s_gi.height);
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

static void poll_pad_live(void)
{
    ag_event_t ev;
    while (ag_poll_event(&ev, 0)) {
        (void)handle_session_ev(&ev);
    }
    if (!ag_focused()) {
        for (unsigned i = 0; i < sizeof(k_live_map) / sizeof(k_live_map[0]);
             i++) {
            gwenesis_io_pad_release_button(0, k_live_map[i].md);
        }
        return;
    }
    for (unsigned i = 0; i < sizeof(k_live_map) / sizeof(k_live_map[0]); i++) {
        if (ag_btnp(0, k_live_map[i].ag)) {
            gwenesis_io_pad_press_button(0, k_live_map[i].md);
        } else {
            gwenesis_io_pad_release_button(0, k_live_map[i].md);
        }
    }
    /* Old 3-byte hosts only have sys.pause; btnp(START) already ORs that in. */
    {
        static int was;
        const int q = ag_btnp(0, AG_BTN_QUIT) ? 1 : 0;
#ifdef MD_MENU
        if (q && !was) {
            s_menu_req = 1;   /* the pause menu: quitting is one of its items */
        }
#else
        if (q) {
            s_quit = 1;
        }
#endif
        was = q;
    }
}

static void poll_pad_serial(void)
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
        if (ev.key.keycode == AG_KEY_ESC || ev.key.keycode == AG_KEY_Q) {
#ifdef MD_MENU
            if (ev.key.keycode == AG_KEY_ESC) {
                if (ev.type == AG_EV_KEY_DOWN) {
                    s_menu_req = 1;
                }
                continue;
            }
#endif
            s_quit = 1;
            continue;
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
    static int rendered;   /* the line drawn last (the address-error resume) */
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
    rendered = -1;
#ifdef ARGON_MD_SMALL
    const int here = !md_snd_async; /* the sound side on this core */
#else
    const int here = 1;
#endif
    if (s_run_sound && here) {
        md_sound_begin_frame(lines_per_frame);
    }
#ifdef ARGON_MD_SMALL
    if (!here) {
        md_snd_68k_frame_begin(lines_per_frame, screen_height);
    }
#endif

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
        /*
         * The line is drawn when its picture starts, not when it ends.  A
         * line's 3420 clocks start (here) with its horizontal blank, ~860
         * clocks, where a line interrupt's handler begins; what the 68000
         * writes after that comes while the line is being shown and counts
         * from the next one.  Drawn after the whole line, Road Rash II's road
         * took the next band's vertical scroll a line early - written 1900
         * clocks in - and one line of mountains crossed the sky (host
         * harness, 31 Oct 2026).
         */
        if (render && rendered != scan_line) {
            rendered = scan_line;
            m68k_run(system_clock - VDP_CYCLES_PER_LINE + MD_LINE_HBLANK);
#ifdef ARGON_MD_SMALL
            const int line = md_line_begin(scan_line);
            if (line >= 0) {
                gwenesis_vdp_render_line(line);
                md_line_end(line);
            }
#else
            if (scan_line < screen_height) {
                gwenesis_vdp_render_line(scan_line);
            }
#endif
        }
        m68k_run(system_clock);
        if (profile) {
            t1 = ag_micros();
            m68k_us += (uint64_t)(t1 - t0);
            t0 = t1;
        }

        if (!here) {
            /* the system core runs the Z80 and the chips (md_snd_async.c) */
        } else if (s_run_z80_cpu) {
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

        if (s_run_sound && here && MD_SOUND_LINE_DUE(scan_line, lines_per_frame)) {
            /* Catch YM up to the line clock before draining samples. */
            ym2612_run(system_clock);
            md_sound_line(scan_line, lines_per_frame);
        }
        if (profile) {
            t1 = ag_micros();
            snd_us += (uint64_t)(t1 - t0);
            t0 = t1;
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
#ifdef ARGON_MD_SMALL
            if (md_raster_on) {
                md_raster_visible(); /* before the vertical blank's DMA */
            }
#endif
            if (REG1_VBLANK_INTERRUPT != 0) {
                gwenesis_vdp_status |= STATUS_VIRQPENDING;
                m68k_set_irq(6);
            }
            if (s_run_z80_cpu && here) {
                z80_irq_line(1);
            }
        }
        if (scan_line == (screen_height + 1)) {
            if (s_run_z80_cpu && here) {
                z80_irq_line(0);
            }
        }
    }

    if (s_run_sound && here) {
        md_sound_end_frame();
    }
    /* Rebase the 68000's counter so next frame starts from zero again. */
    m68k_frame_end(system_clock);
#ifdef ARGON_MD_SMALL
    if (!here) {
        /* the frame's log to the system core, which rebases its own clock */
        md_snd_68k_frame_end(lines_per_frame, screen_height);
    } else
#endif
    /* Z80 clock is absolute within the frame; rebase with the 68k. */
    if (zclk >= system_clock) {
        zclk -= system_clock;
    } else {
        zclk = 0;
    }
    if (profile) {
        s_prof_m68k_us += m68k_us;
        s_prof_z80_us += z80_us;
        s_prof_snd_us += snd_us;
        s_prof_vdp_us += vdp_us;
    }
    frame_counter++;
}

#ifdef MD_MENU
/*
 * The Mega Drive's own front end, drawn on the glass a row at a time through
 * the ring's first row - the row the drawer is not using between passes - and
 * sent the way the game's rows are, so it costs no memory of its own.
 *
 * Started with no cartridge, MD shows a window over the drives and folders
 * (md_pick); Esc in a game (the phone pad's MENU, a keyboard's Esc, a pad's
 * quit) pauses it and shows RESUME / SAVE STATE / LOAD STATE / LOAD GAME /
 * EXIT (md_pause_menu).  LOAD GAME gives the machine's memory back and goes to
 * the window again: on the CYD that is what lets the card be mounted again.
 *
 * The font is in flash (--code-rodata .rodata.k_font8x16) and read by words,
 * as the CYD's other tables are: 4 KB of data was the machine a page short.
 */
#include "font8x16.h"
#include "md_savestate.h"

static char s_save_path[96]; /* where SAVE STATE writes */
static int  s_to_pick;       /* LOAD GAME: back to the window after the loop */
static int  s_open_pick;     /* ...with the game window open at once */

#define UI_BG   0x0841u
#define UI_EDGE 0x52aau
#define UI_FG   0xdefbu
#define UI_SEL  0x2b3du
#define UI_NOTE 0xfd20u
#define UI_DIR  0xffe0u
#define UI_BAR  0x0010u

static inline uint8_t font_row(uint8_t ch, int r)
{
    const uint32_t w = ((const uint32_t *)(const void *)k_font8x16[ch])[r >> 2];
    return (uint8_t)(w >> ((r & 3) * 8));
}

static void ui_text(uint16_t *row, int width, int x, int glyph_row, const char *s,
                    uint16_t fg)
{
    for (; *s != '\0' && x + 8 <= width; s++, x += 8) {
        const uint8_t bits = font_row((uint8_t)*s, glyph_row);
        for (int b = 0; b < 8; b++) {
            if (bits & (1u << b)) { /* bit 0 is the leftmost pixel (font8x16.h) */
                row[x + b] = fg;
            }
        }
    }
}

/* the row buffer the front end draws in: the ring's first row */
static uint16_t *ui_row(void)
{
    return (s_ring_n > 0 && !s_headless) ? (uint16_t *)s_ring[0] : NULL;
}

/* one glass row `y` (from the glass's top), `w` wide, centred */
static void ui_send(uint16_t *row, int y, int w)
{
    md_present_rows(row, y, 1, w, s_gi.height);
}

/* ---- the pause menu ---------------------------------------------------- */

#define MENU_W     232
#define MENU_ITEMS 5
#define MENU_ROWH  18
#define MENU_H     (MENU_ROWH * (MENU_ITEMS + 2) + 8)

static const char *const k_menu_items[MENU_ITEMS] = {
    "RESUME", "SAVE STATE", "LOAD STATE", "LOAD GAME", "EXIT",
};

/* One row of the box, `y` from its top. */
static void menu_row(uint16_t *row, int y, int sel, const char *note)
{
    const bool edge = (y < 2 || y >= MENU_H - 2);
    for (int x = 0; x < MENU_W; x++) {
        row[x] = (edge || x < 2 || x >= MENU_W - 2) ? UI_EDGE : UI_BG;
    }
    if (edge) {
        return;
    }
    const int slot = (y - 4) / MENU_ROWH;   /* 0: the title, then the items */
    const int gy = (y - 4) % MENU_ROWH - 1;
    if (slot >= 1 && slot <= MENU_ITEMS && slot - 1 == sel) {
        for (int x = 4; x < MENU_W - 4; x++) {
            row[x] = UI_SEL;
        }
    }
    if (gy < 0 || gy >= AG_FONT8X16_H) {
        return;
    }
    if (slot == 0) {
        ui_text(row, MENU_W, 12, gy, "PAUSED", UI_NOTE);
    } else if (slot <= MENU_ITEMS) {
        ui_text(row, MENU_W, 20, gy, k_menu_items[slot - 1], UI_FG);
    } else if (note != NULL) {
        ui_text(row, MENU_W, 12, gy, note, UI_NOTE);
    }
}

static void menu_draw(int sel, const char *note)
{
    uint16_t *const row = ui_row();
    if (row == NULL) {
        return;
    }
    const int top = ((int)s_gi.height - MENU_H) / 2;
    const int game_top = ((int)s_gi.height - screen_height) / 2;
    for (int y = 0; y < MENU_H; y++) {
        menu_row(row, y, sel, note);
        ui_send(row, top + y, MENU_W);
        const int line = top + y - game_top;
        if (line >= 0 && line < MD_HASH_LINES) {
            s_line_hash[line] = 0xffffffffu; /* the game's row goes again */
        }
    }
}

/* The buttons the front end answers, as a mask of what is held. */
#define MB_UP   0x01u
#define MB_DOWN 0x02u
#define MB_PICK (0x04u | 0x08u | 0x10u)
#define MB_BACK 0x20u
#define MB_MENU 0x40u

static unsigned menu_buttons(void)
{
    static const int k_ids[] = {AG_BTN_UP, AG_BTN_DOWN, AG_BTN_B1, AG_BTN_C,
                                AG_BTN_START, AG_BTN_B2, AG_BTN_QUIT};
    unsigned m = 0;
    for (unsigned i = 0; i < sizeof(k_ids) / sizeof(k_ids[0]); i++) {
        if (ag_btnp(0, k_ids[i])) {
            m |= 1u << i;
        }
    }
    return m;
}

/* the next press; up and down repeat while held */
/* a key as the front end reads it (the pad layer's keys, and Backspace) */
static unsigned ui_key(uint16_t k)
{
    switch (k) {
    case AG_KEY_UP:
        return MB_UP;
    case AG_KEY_DOWN:
        return MB_DOWN;
    case AG_KEY_ENTER:
    case AG_KEY_Z:
    case AG_KEY_C:
        return MB_PICK;
    case AG_KEY_X:
    case AG_KEY_BACKSPACE:
        return MB_BACK;
    case AG_KEY_ESC:
        return MB_MENU;
    default:
        return 0;
    }
}

/*
 * The presses from the event queue as well as from the pad layer: a key held
 * shorter than a redraw is gone from the pad layer by the time it is asked
 * (150 ms), and the window lost every few presses that way on the CYD.  A
 * repeat (the phone pad resending a held button) is not a press.
 */
static unsigned ui_press(unsigned *held, uint32_t *since)
{
    ag_event_t ev;
    unsigned   queued = 0;
    while (ag_poll_event(&ev, 0)) {
        if (handle_session_ev(&ev)) {
            continue;
        }
        if (ev.type == AG_EV_KEY_DOWN && !ev.key.repeat) {
            queued |= ui_key(ev.key.keycode);
        }
    }
    const unsigned now = menu_buttons();
    const unsigned press = (now & ~*held) | queued;
    const uint32_t t = ag_millis();
    *held = now;
    if (press != 0u) {
        *since = t;
        return press;
    }
    if ((now & (MB_UP | MB_DOWN)) && t - *since > 350u) {
        *since = t - 250u;            /* then every 100 ms */
        return now & (MB_UP | MB_DOWN);
    }
    if (now == 0u) {
        *since = t;
    }
    return 0;
}

static void md_wait_drawer_idle(void)
{
    /*
     * The pass under way finishes, and the drawer waits for the next frame.
     * Without the hold a frame finished just before the game stopped was
     * drawn after this returned - over the city's top rows (3 Nov 2026).
     */
    s_draw_hold = 1;
    if (!s_sysdraw) {
        for (int i = 0; i < 500 && s_pass_h != 0; i++) {
            ag_delay(2);
        }
        return;
    }
    for (int i = 0, calm = 0; i < 500 && calm < 3; i++) {
        calm = (s_pass_h == 0 && s_drawer_parked) ? calm + 1 : 0;
        ag_delay(2);
    }
}

/* the ring lent as scratch: two rows and more, idle between passes */
static void *ui_scratch(size_t *len)
{
#ifdef MD_PSRAM_PAGES
    /*
     * Internal memory there is to lend, and the card's reads land in the
     * buffer by DMA: the ring's rows, wherever they were, read back as
     * nonsense on the S3-CAM (3 Nov 2026).
     */
    static void *s_own;
    if (s_own == NULL) {
        s_own = ag_malloc_caps(MD_STATE_SCRATCH, AG_MEM_FAST | AG_MEM_DMA);
    }
    if (s_own != NULL) {
        *len = MD_STATE_SCRATCH;
        return s_own;
    }
#endif
    if (s_ring_n < 2) {
        *len = 0;
        return NULL;
    }
    *len = (size_t)((uint8_t *)s_ring_end - (uint8_t *)s_ring[0]);
    return s_ring[0];
}

static void md_pause_menu(void)
{
    md_wait_drawer_idle();
    ag_printf("md: paused\n");
    int         sel = 0;
    const char *note = NULL;
    unsigned    held = menu_buttons(); /* what opened it is not a press */
    {
        ag_event_t ev;   /* nor is its key, still in the queue */
        while (ag_poll_event(&ev, 0)) {
            (void)handle_session_ev(&ev);
        }
    }
    uint32_t    since = ag_millis();
    menu_draw(sel, note);
    for (;;) {
        if (s_quit) {
            return;
        }
        const unsigned press = ui_press(&held, &since);
        int act = -1;
        if (press & MB_UP) {
            sel = (sel + MENU_ITEMS - 1) % MENU_ITEMS;
            note = NULL;
        } else if (press & MB_DOWN) {
            sel = (sel + 1) % MENU_ITEMS;
            note = NULL;
        } else if (press & (MB_BACK | MB_MENU)) {
            act = 0;
        } else if (press & MB_PICK) {
            act = sel;
        }
        if (act == 0) {
            break;
        }
        if (act == 1 || act == 2) {
            size_t len = 0;
            void *const scratch = ui_scratch(&len);
            /*
             * The file system wants a buffer for the file, and the CYD has a
             * couple of kilobytes free while it plays: the raster record's
             * are lent for the length of it and the record made again after.
             */
            const int had_raster = md_raster_on;
            if (had_raster) {
                md_raster_free(ag_free);
            }
            md_sound_close(); /* the converter's buffers, whole blocks */
            ag_meminfo_t mi;
            ag_meminfo(&mi);
            const int r = (act == 1) ? md_state_save(s_save_path, scratch, len)
                                     : md_state_load(s_save_path, scratch, len);
            md_sound_init();
            if (had_raster) {
                md_raster_on = md_raster_init(md_crumb_alloc);
            }
            ag_printf("md: %s %s: %d (%s %d, %u free)\n",
                      (act == 1) ? "save to" : "load from", s_save_path, r,
                      md_state_why, md_state_err, (unsigned)mi.fast_free);
            if (act == 2 && r == MD_STATE_OK) {
                break;
            }
            if (act == 2 && r == MD_STATE_BAD) {
                reset_emulation(); /* half loaded is no machine at all */
            }
            note = (act == 1) ? ((r == MD_STATE_OK) ? "SAVED" : "COULD NOT SAVE")
                   : (r == MD_STATE_NO_FILE) ? "NO SAVE YET" : "COULD NOT LOAD";
        } else if (act == 3 || act == 4) {
            /* LOAD GAME: the main screen with the game window open; EXIT:
             * the main screen - MD itself is left from there (MENU) */
            s_to_pick = 1;
            s_open_pick = (act == 3);
            s_quit = 1;
            return;
        }
        if (press != 0u) {
            menu_draw(sel, note);
        }
        ag_delay(20);
    }
    /* the game's own rows over the box */
    memset(s_line_hash, 0xff, sizeof(s_line_hash));
    ag_printf("md: resumed\n");
}

/*
 * Where the state goes: beside the cartridge where there is memory to keep
 * the card mounted (PSRAM boards); C:\ on the CYD, whose card is let go
 * while the machine runs.  The cartridge's name with .sav.
 */
static void md_save_path_for(const char *rom)
{
    s_save_path[0] = '\0';
    if (rom == NULL) {
        return;
    }
    const char *base = rom;
    for (const char *p = rom; *p != '\0'; p++) {
        if (*p == '\\' || *p == '/' || *p == ':') {
            base = p + 1;
        }
    }
#ifdef MD_PSRAM_PAGES
    (void)snprintf(s_save_path, sizeof(s_save_path) - 5u, "%s", rom);
#else
    (void)snprintf(s_save_path, sizeof(s_save_path) - 5u, "C:\\%s", base);
#endif
    char *dot = strrchr(s_save_path, '.');
    char *sep = strrchr(s_save_path, '\\');
    if (dot != NULL && (sep == NULL || dot > sep)) {
        *dot = '\0';
    }
    strcat(s_save_path, ".sav");
}

/* ---- the window over the drives ---------------------------------------- */

#define PK_PATH 160
#define PK_NAME 48

static char s_pk_dir[PK_PATH];     /* "" is the list of drives */
static char s_pk_rom[PK_PATH];     /* what was chosen */

static char pk_up(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

static bool pk_ends(const char *s, const char *ext)
{
    const size_t n = strlen(s), e = strlen(ext);
    if (n < e) {
        return false;
    }
    for (size_t i = 0; i < e; i++) {
        if (pk_up(s[n - e + i]) != ext[i]) {
            return false;
        }
    }
    return true;
}

static bool pk_raw(const char *n)
{
    return pk_ends(n, ".BIN") || pk_ends(n, ".MD") || pk_ends(n, ".GEN");
}

static bool pk_rom(const char *n)
{
    return pk_ends(n, ".SWP") || pk_raw(n);
}

static void pk_join(char *out, size_t cap, const char *dir, const char *name)
{
    const size_t d = strlen(dir);
    const bool sep = d > 0 && dir[d - 1] != '\\' && dir[d - 1] != '/';
    (void)snprintf(out, cap, "%s%s%s", dir, sep ? "\\" : "", name);
}

static void pk_parent(char *dir)
{
    size_t n = strlen(dir);
    if (n <= 3) {
        dir[0] = '\0';
        return;
    }
    if (dir[n - 1] == '\\') {
        dir[--n] = '\0';
    }
    while (n > 0 && dir[n - 1] != '\\') {
        n--;
    }
    dir[n] = '\0';
    if (n == 2) {
        dir[2] = '\\';
        dir[3] = '\0';
    }
}

/*
 * Entry `want` of the window (folders first, then cartridges, as the folder
 * lists them - nothing is held, so nothing is sorted), or the count with
 * want < 0.  Index 0 is "..".
 */
static int pk_entry(int want, char *name, bool *dir)
{
    int n = 0;
    if (s_pk_dir[0] == '\0') {
        for (char d = 'A'; d <= 'Z'; d++) {
            if (d == 'D' || d == 'T') {
                continue;        /* devices and memory, not files */
            }
            char root[4] = {d, ':', '\\', 0};
            const ag_handle_t h = ag_opendir(root);
            if (h < 0) {
                continue;
            }
            (void)ag_closedir(h);
            if (n == want) {
                (void)snprintf(name, PK_NAME, "%s", root);
                *dir = true;
                return n;
            }
            n++;
        }
        return n;
    }
    if (want == 0) {
        (void)snprintf(name, PK_NAME, "..");
        *dir = true;
        return 0;
    }
    n = 1;
    for (int pass = 0; pass < 2; pass++) {
        const ag_handle_t h = ag_opendir(s_pk_dir);
        if (h < 0) {
            return n;
        }
        ag_dirent_t e;
        while (ag_readdir(h, &e) == AG_OK) {
            const bool d = (e.st.attr & AG_A_DIR) != 0;
            if (e.name[0] == '.' || (pass == 0) != d || (!d && !pk_rom(e.name)) ||
                strcmp(e.name, "System Volume Information") == 0) {
                continue;
            }
            if (n == want) {
                (void)snprintf(name, PK_NAME, "%s", e.name);
                *dir = d;
                (void)ag_closedir(h);
                return n;
            }
            n++;
        }
        (void)ag_closedir(h);
    }
    return n;
}

/*
 * A plain cartridge made ready: "SEGA" at 0x100 is a dump as it comes and is
 * swapped into the .SWP beside it once (MD reads the cartridge out of flash
 * in place, a word at a time); "ESAG" is swapped already and plays as it is
 * - swapped twice it was UGSNAT REHORSE and did not start (3 Nov 2026).
 */
static bool pk_ready(const char *src, char *out, size_t cap)
{
    {
        const ag_handle_t h = ag_open(src, AG_O_RDONLY);
        if (h < 0) {
            return false;
        }
        uint8_t hdr[4] = {0};
        const bool got = ag_seek(h, 0x100, AG_SEEK_SET) >= 0 && ag_read(h, hdr, 4) == 4;
        (void)ag_close(h);
        if (!got) {
            return false;
        }
        (void)snprintf(out, cap, "%s", src);
        if (memcmp(hdr, "ESAG", 4) == 0) {
            return true;
        }
        if (memcmp(hdr, "SEGA", 4) != 0) {
            return false;
        }
    }
    char *dot = strrchr(out, '.');
    if (dot == NULL || (size_t)(dot - out) + 5u > cap) {
        return false;
    }
    memcpy(dot, ".SWP", 5);
    {
        const ag_handle_t h = ag_open(out, AG_O_RDONLY);
        if (h >= 0) {
            (void)ag_close(h);
            return true;   /* made before */
        }
    }
    const ag_handle_t in = ag_open(src, AG_O_RDONLY);
    const ag_handle_t o = ag_open(out, AG_O_WRONLY | AG_O_CREATE | AG_O_TRUNC);
    uint8_t *buf = (uint8_t *)ag_malloc(2048);
    bool ok = in >= 0 && o >= 0 && buf != NULL;
    uint32_t done = 0;
    while (ok) {
        const int32_t n = ag_read(in, buf, 2048);
        if (n <= 0) {
            ok = (n == 0);
            break;
        }
        for (int32_t k = 0; k + 1 < n; k += 2) {
            const uint8_t t = buf[k];
            buf[k] = buf[k + 1];
            buf[k + 1] = t;
        }
        ok = ag_write(o, buf, (size_t)n) == n;
        done += (uint32_t)n;
    }
    if (buf != NULL) {
        ag_free(buf);
    }
    if (in >= 0) {
        (void)ag_close(in);
    }
    if (o >= 0) {
        (void)ag_close(o);
    }
    if (!ok) {
        (void)ag_unlink(out);
    }
    return ok;
}

/* ---- the city, and windows over it ----------------------------------- */

/*
 * A city at night (tools/gen_md_city.py: 16 colours, 7.6 KB packed, in flash
 * via --code-rodata .rodata.k_city and read by words as the font is) behind
 * everything the front end shows outside a game: unpacked a row at a time
 * into the ring's first row, something drawn over the row, and sent.
 */
#include "md_city.h"

static inline uint8_t city_byte(uint32_t i)
{
    const uint32_t w = ((const uint32_t *)(const void *)k_city)[i >> 2];
    return (uint8_t)(w >> ((i & 3u) * 8u));
}

/* the city's rows y0..y1-1 to the glass, `over` drawing on each first */
static void city_paint(int y0, int y1, void (*over)(uint16_t *row, int y))
{
    uint16_t *const row = ui_row();
    if (row == NULL) {
        return;
    }
    uint32_t pos = 0;
    for (int y = 0; y < MD_CITY_H_ && y < y1; y++) {
        int x = 0;
        while (x < MD_CITY_W) {
            const uint8_t h = city_byte(pos++);
            const int n = (h < 0x80u) ? h + 1 : h - 0x7f;
            const uint8_t rep = (h < 0x80u) ? 0u : city_byte(pos++);
            for (int k = 0; k < n && x < MD_CITY_W; k++) {
                const uint8_t p = (h < 0x80u) ? city_byte(pos++) : rep;
                row[x++] = k_city_pal[p >> 4];
                row[x++] = k_city_pal[p & 15u];
            }
        }
        if (y >= y0) {
            if (over != NULL) {
                over(row, y);
            }
            ui_send(row, y, MD_CITY_W);
        }
    }
    if (y0 == 0 && s_gi.fb != NULL) {
        /* the whole picture to the panel once more, in one piece: after a
         * game the S3-CAM's ST7789 kept a quarter of the game's last frame
         * over the city's top though the framebuffer had the city (3 Nov) */
        ag_gfx_flush((uint16_t)((s_gi.width - MD_CITY_W) / 2), 0,
                     MD_CITY_W, (uint32_t)(y1 < MD_CITY_H_ ? y1 : MD_CITY_H_));
    }
}

/*
 * A window over the city, the pause menu's look: a title, up to WIN_LINES
 * lines with one lit, and a line for a note or a hint.
 */
#define WIN_X0    24
#define WIN_W     272
#define WIN_Y0    28
#define WIN_LINES 9
#define WIN_COLS  (WIN_W / 8 - 2)
#define WIN_H     (4 + (WIN_LINES + 2) * 16 + 4)

static struct {
    char        title[WIN_COLS + 1];
    char        line[WIN_LINES][WIN_COLS + 1];
    uint8_t     dir[WIN_LINES];
    int         n, sel;              /* lines in use, the lit one (-1 none) */
    const char *foot;
} s_win;

static void win_over(uint16_t *row, int y)
{
    if (y < WIN_Y0 || y >= WIN_Y0 + WIN_H) {
        return;
    }
    uint16_t *const r = row + WIN_X0;
    const int wy = y - WIN_Y0;
    const bool edge = (wy < 2 || wy >= WIN_H - 2);
    for (int x = 0; x < WIN_W; x++) {
        r[x] = (edge || x < 2 || x >= WIN_W - 2) ? UI_EDGE : UI_BG;
    }
    if (edge || wy < 4) {
        return;
    }
    const int line = (wy - 4) / 16, gy = (wy - 4) % 16;
    if (line == 0) {
        ui_text(r, WIN_W, 8, gy, s_win.title, UI_NOTE);
    } else if (line <= WIN_LINES && line - 1 < s_win.n) {
        const int i = line - 1;
        if (i == s_win.sel) {
            for (int x = 4; x < WIN_W - 4; x++) {
                r[x] = UI_SEL;
            }
        }
        ui_text(r, WIN_W, 8, gy, s_win.line[i],
                (i == s_win.sel) ? UI_FG : s_win.dir[i] ? UI_DIR : UI_FG);
    } else if (line == WIN_LINES + 1 && s_win.foot != NULL) {
        ui_text(r, WIN_W, 8, gy, s_win.foot, UI_NOTE);
    }
}

/* the whole window, or only line `i`'s rows */
static void win_paint(void)
{
    city_paint(WIN_Y0, WIN_Y0 + WIN_H, win_over);
}

static void win_paint_line(int i)
{
    const int y = WIN_Y0 + 4 + (i + 1) * 16;
    city_paint(y, y + 16, win_over);
}

static void win_paint_foot(void)
{
    win_paint_line(WIN_LINES);
}

/* ---- the game window over the city ------------------------------------- */

/* the window's lines for entries top..top+WIN_LINES-1 */
static void pk_fill(int top, int count, int sel)
{
    (void)snprintf(s_win.title, sizeof(s_win.title), "LOAD GAME %s",
                   s_pk_dir[0] ? s_pk_dir : "(drives)");
    s_win.n = 0;
    for (int r = 0; r < WIN_LINES && top + r < count; r++) {
        char name[PK_NAME];
        bool dir = false;
        if (pk_entry(top + r, name, &dir) != top + r) {
            break;
        }
        (void)snprintf(s_win.line[r], sizeof(s_win.line[r]), dir ? "[%s]" : "%s", name);
        s_win.dir[r] = dir ? 1u : 0u;
        s_win.n++;
    }
    s_win.sel = sel - top;
}

/* The window: 1 with a cartridge in s_pk_rom, 0 back to the main screen. */
static int md_pick(void)
{
    if (ui_row() == NULL) {
        return 0;
    }
    if (s_pk_dir[0] == '\0') {
        const ag_handle_t h = ag_opendir("A:\\");
        if (h >= 0) {
            (void)ag_closedir(h);
            (void)snprintf(s_pk_dir, sizeof(s_pk_dir), "A:\\");
        }
    }
    static const char k_hint[] = "A:OPEN  B:UP  MENU:BACK";
    int sel = 0, top = 0, count = 0, shown_sel = -1, shown_top = -1;
    bool fresh = true;
    unsigned held = menu_buttons();
    uint32_t since = ag_millis();
    s_win.foot = k_hint;
    for (;;) {
        char name[PK_NAME];
        bool dir = false;
        if (fresh) {
            count = pk_entry(-1, name, &dir);
        }
        if (sel >= count) {
            sel = count ? count - 1 : 0;
        }
        if (sel < top) {
            top = sel;
        } else if (sel >= top + WIN_LINES) {
            top = sel - WIN_LINES + 1;
        }
        if (fresh || top != shown_top) {
            pk_fill(top, count, sel);
            win_paint();
        } else if (sel != shown_sel) {
            s_win.sel = sel - top;      /* a move inside the page: two lines */
            win_paint_line(shown_sel - top);
            win_paint_line(sel - top);
        }
        if (s_win.foot != k_hint) {
            win_paint_foot();
            s_win.foot = k_hint;        /* a note stays until the next press */
        }
        fresh = false;
        shown_sel = sel;
        shown_top = top;
        unsigned p = 0;
        while ((p = ui_press(&held, &since)) == 0u) {
            if (s_quit) {
                return 0;
            }
            if (s_ui_lost) {
                s_ui_lost = 0;
                city_paint(0, MD_CITY_H_, NULL);
                win_paint();
            }
            ag_delay(20);
        }
        if (p & MB_UP) {
            sel = count ? (sel + count - 1) % count : 0;
        } else if (p & MB_DOWN) {
            sel = count ? (sel + 1) % count : 0;
        } else if (p & MB_MENU) {
            return 0;
        } else if ((p & (MB_BACK | MB_PICK)) && count > 0) {
            if (pk_entry(sel, name, &dir) != sel) {
                continue;
            }
            if ((p & MB_BACK) || strcmp(name, "..") == 0) {
                if (s_pk_dir[0] != '\0') {
                    pk_parent(s_pk_dir);
                    sel = top = 0;
                    fresh = true;
                }
                continue;
            }
            if (dir) {
                char next[PK_PATH];
                if (s_pk_dir[0] == '\0') {
                    (void)snprintf(next, sizeof(next), "%s", name);
                } else {
                    pk_join(next, sizeof(next), s_pk_dir, name);
                }
                (void)snprintf(s_pk_dir, sizeof(s_pk_dir), "%s", next);
                sel = top = 0;
                fresh = true;
                continue;
            }
            char rom[PK_PATH];
            pk_join(rom, sizeof(rom), s_pk_dir, name);
            s_win.foot = pk_raw(name) ? "PREPARING..." : "LOADING...";
            win_paint_foot();
            if (pk_raw(name)) {
                if (!pk_ready(rom, s_pk_rom, sizeof(s_pk_rom))) {
                    s_win.foot = "NOT A CARTRIDGE, OR CARD FULL";
                    continue;
                }
            } else {
                (void)snprintf(s_pk_rom, sizeof(s_pk_rom), "%s", rom);
            }
            return 1;
        }
    }
}

/* ---- the main screen --------------------------------------------------- */

#define HOME_ITEMS 2
static const char *const k_home_items[HOME_ITEMS] = {"LOAD GAME", "SETTINGS"};
#define HOME_Y0    194             /* the first item's top row */
#define HOME_STEP  20
static int s_home_sel;

static void home_over(uint16_t *row, int y)
{
    for (int i = 0; i < HOME_ITEMS; i++) {
        const int gy = y - (HOME_Y0 + i * HOME_STEP);
        if (gy >= 0 && gy < AG_FONT8X16_H) {
            char buf[24];
            (void)snprintf(buf, sizeof(buf), (i == s_home_sel) ? "> %s <" : "%s",
                           k_home_items[i]);
            const int x = (MD_CITY_W - (int)strlen(buf) * 8) / 2;
            ui_text(row, MD_CITY_W, x + 1, gy, buf, 0x0000u);
            ui_text(row, MD_CITY_W, x, gy, buf,
                    (i == s_home_sel) ? 0x07ffu : 0xbdf7u);
        }
    }
}

static void md_settings(void)
{
    (void)snprintf(s_win.title, sizeof(s_win.title), "SETTINGS");
    (void)snprintf(s_win.line[0], sizeof(s_win.line[0]), "NOTHING TO SET YET");
    s_win.dir[0] = 0;
    s_win.n = 1;
    s_win.sel = -1;
    s_win.foot = "B OR MENU: BACK";
    win_paint();
    unsigned held = menu_buttons();
    uint32_t since = ag_millis();
    for (;;) {
        const unsigned p = ui_press(&held, &since);
        if ((p & (MB_BACK | MB_MENU | MB_PICK)) || s_quit) {
            return;
        }
        if (s_ui_lost) {
            s_ui_lost = 0;
            city_paint(0, MD_CITY_H_, NULL);
            win_paint();
        }
        ag_delay(20);
    }
}

/* The main screen: 1 with a cartridge in s_pk_rom, 0 to leave MD (MENU). */
static int md_home(void)
{
    if (ui_row() == NULL) {
        return 0;
    }
    unsigned held = menu_buttons();
    uint32_t since = ag_millis();
    {
        ag_event_t ev;
        while (ag_poll_event(&ev, 0)) {
            (void)handle_session_ev(&ev);
        }
    }
    ag_printf("md: home\n");
    s_ui_lost = 0;
    bool whole = true;
    for (;;) {
        if (s_open_pick) {
            s_open_pick = 0;
            s_home_sel = 0;
            city_paint(0, MD_CITY_H_, NULL);
            if (md_pick()) {
                return 1;
            }
            whole = true;
            held = menu_buttons();
            continue;
        }
        if (whole) {
            city_paint(0, MD_CITY_H_, home_over);
            whole = false;
            ag_printf("md: home painted\n");
        } else {
            city_paint(HOME_Y0, HOME_Y0 + HOME_ITEMS * HOME_STEP, home_over);
        }
        unsigned p = 0;
        while ((p = ui_press(&held, &since)) == 0u) {
            if (s_quit) {
                return 0;
            }
            if (s_ui_lost) {
                s_ui_lost = 0;
                whole = true;
                break;
            }
            ag_delay(20);
        }
        ag_printf("md: home press %x\n", p);   /* what reached it, for a test */
        if (p & (MB_UP | MB_DOWN)) {
            s_home_sel ^= 1;
        } else if (p & MB_MENU) {
            return 0;
        } else if (p & MB_PICK) {
            if (s_home_sel == 0) {
                city_paint(0, MD_CITY_H_, NULL);   /* the items out from under */
                if (md_pick()) {
                    return 1;
                }
            } else {
                md_settings();
            }
            if (s_quit) {
                return 0;
            }
            held = menu_buttons();
            whole = true;
        }
    }
}
#endif

int ag_main(int argc, char **argv)
{
    const char *rom = NULL;
#ifdef ARGON_MD_SMALL
    /* the CYD has a speaker, and a game is meant to be heard: `nosound` or
     * `mock` to measure without one */
    const char *sound_path = "pcm0";
#else
    const char *sound_path = "mock";
#endif
    int         frames = -1; /* <0: until Esc/Q */
    int         present_div = 1;
    int         stats = 0;
    int         want_livepad = 1;
    int         wait_card = 0;
    /* rows of the ring both cores draw into (md_send): a call to the glass
     * costs about the same whatever it carries, so rows go four at a time or
     * to the ring's end, and six leave room to draw ahead while four go
     * (Sonic with sound: 17 whole passes a second at 2 rows and the system
     * core alone, 22 at 4, 30-34 at 6 with both cores, 30 Oct 2026); `ringN`
     * asks for N (2..16), `noring` for none */
    int         ring_rows = 6;

    for (int i = 1; i < argc; i++) {
        if (arg_eq(argv[i], "waitcard")) {
            wait_card = 1;
            continue;
        }
#ifdef MD_Z80PROF
        if (arg_eq(argv[i], "z80bench")) {
            s_z80bench = 1;
            continue;
        }
#endif
#ifdef ARGON_MD_SMALL
        if ((argv[i][0] == 'r' || argv[i][0] == 'R') &&
            (argv[i][1] == 'i' || argv[i][1] == 'I') &&
            (argv[i][2] == 'n' || argv[i][2] == 'N') &&
            (argv[i][3] == 'g' || argv[i][3] == 'G') &&
            looks_like_number(argv[i] + 4)) {
            /* ringN: N rows of ring, 2..16 */
            ring_rows = parse_int(argv[i] + 4, ring_rows);
            ring_rows = ring_rows < 2 ? 2 : ring_rows > MD_RING_MAX ? MD_RING_MAX : ring_rows;
            continue;
        }
        if (argv[i][0] == 's' && argv[i][1] == 'h' && argv[i][2] == 'o' &&
            argv[i][3] == 't' && argv[i][4] >= '0' && argv[i][4] <= '9') {
            s_shot_at = 0;
            for (const char *c = argv[i] + 4; *c >= '0' && *c <= '9'; c++) {
                s_shot_at = s_shot_at * 10u + (uint32_t)(*c - '0');
            }
            continue;
        }
        if (arg_eq(argv[i], "nohelp")) {
            s_help = 0;        /* the system core draws every line alone */
            continue;
        }
        if (arg_eq(argv[i], "nosysdraw")) {
            s_sysdraw = 0;     /* the emulating core draws, as before */
            continue;
        } else if (arg_eq(argv[i], "noring")) {
            ring_rows = 0;
            continue;
        } else if (arg_eq(argv[i], "headless")) {
            s_headless = 1;
            continue;
        }
#endif
        if (arg_eq(argv[i], "fps20")) {
            present_div = 3;
        } else if (arg_eq(argv[i], "fps15")) {
            present_div = 4;
        } else if (arg_eq(argv[i], "fps10")) {
            present_div = 6;
        } else if (arg_eq(argv[i], "fps30")) {
            present_div = 2;
        } else if (arg_eq(argv[i], "fps60")) {
            present_div = 1;
        } else if (arg_eq(argv[i], "stats")) {
            stats = 1;
        } else if (arg_eq(argv[i], "profile")) {
            /* m68k/z80/snd/vdp per frame, every stats window: the timing
             * itself costs four clock reads a line */
            s_profile_frames = 1;
            stats = 1;
        } else if (arg_eq(argv[i], "sndhere")) {
            s_snd_here = 1; /* the CYD: Z80 and chips on this core, as before */
        } else if (arg_eq(argv[i], "hifi")) {
            md_sound_set_hifi(1); /* the CYD: the chips at the whole rate */
        } else if (arg_eq(argv[i], "noz80")) {
            s_run_z80_cpu = 0;
        } else if (arg_eq(argv[i], "nosound")) {
            s_run_sound = 0;
            sound_path = "mock";
        } else if (arg_eq(argv[i], "nolivepad")) {
            want_livepad = 0;
        } else if (arg_eq(argv[i], "livepad")) {
            want_livepad = 1;
        } else if (arg_eq(argv[i], "mock") || arg_eq(argv[i], "pcmnull")) {
            sound_path = "pcmnull";
        } else if (arg_eq(argv[i], "wav")) {
            sound_path = "a:\\md.wav";
        } else if (arg_eq(argv[i], "net") || arg_eq(argv[i], "tcp") ||
                   arg_eq(argv[i], "pcmvirt")) {
            sound_path = "pcmvirt";
        } else if (arg_eq(argv[i], "mix") || arg_eq(argv[i], "pcmmix")) {
            sound_path = "pcmmix";
        } else if (arg_eq(argv[i], "audio") || arg_eq(argv[i], "i2s") ||
                   arg_eq(argv[i], "pcm0")) {
            sound_path = "pcm0";
        } else if ((argv[i][0] == 'n' || argv[i][0] == 'N') &&
                   (argv[i][1] == 'e' || argv[i][1] == 'E') &&
                   (argv[i][2] == 't' || argv[i][2] == 'T') &&
                   argv[i][3] == ':') {
            sound_path = "pcmvirt"; /* port owned by PCMVIRT.SYS */
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

    /*
     * The system cruises at a lower clock while nothing says otherwise (160 of
     * 240 MHz on the ESP32), and a console paced by its own frame clock does
     * not fit in two thirds of the speed.
     */
    (void)ag_power_declare(AG_POWER_FIT_FULL_ONLY, "a 60 Hz console");

#ifdef ARGON_MD_SMALL
    size_t         rom_size = 0;
    /*
     * The cartridge first: staging it into flash takes a little of the kernel's
     * own memory (the map table, the flash driver), which is gone once the
     * machine's RAM is allocated.
     */
    unsigned char *rom_data = NULL;
#ifdef MD_MENU
    int ejected = 0;
md_next_game:
    /*
     * No cartridge named: the window over the drives, on a glass taken now -
     * the ring and the screen before the machine, which comes once a
     * cartridge is chosen (and, on the CYD, once the card has let go).
     */
    if (rom == NULL && !s_headless) {
        gwenesis_vdp_set_buffer_stride(0);
        md_sync_internal();
        if (s_presenter == NULL && ring_rows > 0) {
            md_ring_start(ring_rows);
        }
        if (s_ring_n == 0 || ag_gfx_acquire(&s_gi) != AG_OK) {
            ag_printf("md: no glass for the game window\n");
            return 1;
        }
        if (!md_home()) {
            md_ring_stop();
            ag_gfx_release();
            return 0;
        }
        rom = s_pk_rom;
    }
#endif
    rom_data = (rom != NULL) ? map_rom(rom, &rom_size) : make_tiny_rom(&rom_size);
#ifdef MD_MENU
    md_save_path_for(rom);
#ifndef MD_PSRAM_PAGES
    /* before the machine takes the memory: the CYD cannot open it mid-game.
     * A PSRAM board opens it for each save and load - kept open on the
     * S3-CAM's FAT card, a load read back BAD (3 Nov 2026, not run down) */
    if (rom_data != NULL && rom != NULL) {
        (void)md_state_keep(s_save_path);
    }
#endif
#ifndef MD_PSRAM_PAGES
    /*
     * The CYD: the cartridge is in flash now, and the card's 44 KB are the
     * machine's - let it go (ABI 0.77), and mount it again for the next game.
     */
    if (rom_data != NULL && rom != NULL && !wait_card &&
        (rom[0] == 'a' || rom[0] == 'A') && rom[1] == ':' &&
        ag_media(false) == AG_OK) {
        ejected = 1;
        ag_printf("md: the card let go for the machine\n");
    }
#endif
#endif
    /*
     * `waitcard`: the cartridge now reads out of flash, and the card's file
     * system holds 11 KB the machine needs.  An application cannot release the
     * card itself, so it waits for `eject` (typed, or the next line of a .bat)
     * - and goes on after 30 s either way.
     */
    if (rom_data != NULL && rom != NULL && wait_card) {
        ag_meminfo_t mi;
        ag_meminfo(&mi);
        const size_t before = mi.fast_free;
        ag_printf("md: waiting for the card to be released (eject)\n");
        for (int t = 0; t < 150; t++) {
            ag_delay(200);
            ag_meminfo(&mi);
            if (mi.fast_free >= before + 8u * 1024u) {
                ag_printf("md: card released, %u KB back\n",
                          (unsigned)((mi.fast_free - before) / 1024u));
                break;
            }
        }
    }
    if (rom_data == NULL) {
        return 1;
    }
    /*
     * Lines are drawn one row at a time - a stride of zero, and md_line_begin
     * says which row.  Without a display (or with `headless`) the VDP's work
     * is the same and the row is s_line, never sent.
     */
    gwenesis_vdp_set_buffer_stride(0);
    /* The ring before the machine's pages; the glass after them - taking the
     * display costs internal memory too, and it is the one that can be had
     * last. */
    md_sync_internal();
    if (!s_headless && ring_rows > 0 && s_presenter == NULL) {
        md_ring_start(ring_rows);
    }
    if (s_ring_n == 0 && !md_line_alloc()) {
        md_ring_stop();
        return 1;
    }
    gwenesis_vdp_set_buffer((s_ring_n > 0) ? s_ring[0] : s_line);
    if (!md_small_alloc()) {
        md_ring_stop();
        return 1;
    }
    if (!s_headless && ag_gfx_acquire(&s_gi) != AG_OK) {
        md_ring_stop();
        s_ring_n = 0;
        s_headless = 1;
        if (!md_line_alloc()) {
            return 1;
        }
        gwenesis_vdp_set_buffer(s_line);
    }
    if (s_headless) {
        ag_printf("md: no picture - measuring\n");
    } else {
        md_clear_glass();
        ag_printf("md: picture on the %ux%u glass, %s\n",
                  (unsigned)s_gi.width, (unsigned)s_gi.height,
                  s_ring_n ? "rows sent from the system core" : "rows sent from here");
        if (s_ring_n) {
            ag_printf("md: a ring of %d rows\n", s_ring_n);
        }
    }
#else
    if (ag_gfx_acquire(&s_gi) != AG_OK) {
        ag_printf("md: gfx acquire failed\n");
        return 1;
    }
    ag_gfx_clear(0x00000000u);
    bind_vdp_to_fb(&s_gi);
#endif

#ifndef ARGON_MD_SMALL
    size_t         rom_size = 0;
    unsigned char *rom_data = (rom != NULL) ? load_rom_file(rom, &rom_size)
                                            : make_tiny_rom(&rom_size);
#endif
    if (rom_data == NULL) {
        ag_gfx_release();
        return 1;
    }

    /* Takes ownership of rom_data and byte-swaps it in place. */
    load_cartridge(rom_data, rom_size);
    power_on();
    reset_emulation();
#ifdef MD_Z80PROF
    if (s_z80bench) {
        md_z80_bench("application core");
        ag_thread_t bt = ag_thread_create(md_z80bench_sys, NULL, "z80bench", 2048, 4,
                                          AG_THREAD_SYS_CORE);
        if (bt != NULL) {
            while (!s_z80bench_done) {
                ag_delay(10);
            }
        }
        return 0;
    }
#endif
    {
        static int sound_up;
        if (!sound_up) {
            md_sound_init();
            sound_up = 1;
        }
    }
#ifdef ARGON_MD_SMALL
    /* the Z80 and the chips on the system core, in the picture's thread,
     * when there is a Z80 to run - without one the YM fits beside the 68000
     * here (Sonic: 68000 11 ms + YM 4 of 16.7) and the picture keeps its core.
     * `sndhere` keeps them here anyway. */
    if (s_sysdraw && s_ring_n > 0 && !s_headless && !s_snd_here &&
        s_run_z80_cpu && !md_snd_async) {
        md_snd_async_start(s_run_z80_cpu, s_run_sound);
        ag_printf("md: the Z80 and the sound chips on the system core\n");
    }
    /* the picture drawn after the frame: its raster writes written down, in
     * what the heap has left after the machine's pages */
    md_raster_on = (s_sysdraw && s_ring_n > 0 && !s_headless &&
                    md_raster_init(md_crumb_alloc)) ? 1 : 0;
    if (!md_raster_on && s_sysdraw && s_ring_n > 0 && !s_headless) {
        ag_printf("md: no memory left to follow raster effects\n");
    }
    {
        extern unsigned md_raster_blocks, md_raster_bytes;
        ag_printf("md: raster record from %u blocks, %u bytes; scroll table %s; "
                  "the machine's pages from %d blocks\n", md_raster_blocks,
                  md_raster_bytes, md_raster_hs_mode(), s_blocks);
    }
#endif

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

    if (!s_headless) {
        /* black once more just before the first frame: what was printed
         * while the game started had reached the S3-CAM's rows above and
         * below the picture, which the game never draws (3 Nov 2026) */
        md_clear_glass();
    }
    uint64_t  work_sum = 0;
    uint64_t  emu_sum = 0;
    uint64_t  present_sum = 0;
    uint64_t  work_max = 0;
    uint32_t  window = 0;
    ag_time_t window_t0 = ag_micros();
    ag_time_t next_frame = window_t0;
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
#ifdef MD_MENU
        if (s_menu_req) {
            s_menu_req = 0;
            md_pause_menu();
            next_frame = window_t0 = ag_micros();
            work_sum = emu_sum = present_sum = work_max = 0;
            window = 0;
            continue;
        }
#endif

        const ag_time_t f0 = ag_micros();

        const int show = (present_div <= 1) || ((ran % present_div) == 0);
        s_inframe_left = MD_INFRAME_LINES;
#ifdef ARGON_MD_SMALL
        if (md_raster_on) {
            md_raster_begin();
        }
        run_frame(show && !(s_sysdraw && s_ring_n > 0));
        if (md_raster_on) {
            md_raster_end();
        }
#else
        run_frame(show);
#endif
        s_last_work_us = (uint32_t)(ag_micros() - f0);
        s_draw_hold = 0;
        s_frames_done++;
        s_frame_no = (uint32_t)ran + 1u;

        const ag_time_t f1 = ag_micros();
#ifndef ARGON_MD_SMALL /* the CYD's bands went out as they were drawn */
        if (show) {
            ag_gfx_flush((uint16_t)s_ox, (uint16_t)s_oy, MD_WIDTH,
                         (uint16_t)screen_height);
        }
#endif
        const ag_time_t f2 = ag_micros();

        const uint64_t work = (uint64_t)(f2 - f0);
        work_sum += work;
#ifdef ARGON_MD_SMALL
        /* the bands went out inside the frame: count them as showing it */
        emu_sum += (uint64_t)(f1 - f0) - s_present_us;
        present_sum += (uint64_t)(f2 - f1) + s_present_us;
        s_present_us = 0;
#else
        emu_sum += (uint64_t)(f1 - f0);
        present_sum += (uint64_t)(f2 - f1);
#endif
        if (work > work_max) {
            work_max = work;
        }
        window++;
        ran++;

        if (stats && (uint64_t)(f2 - window_t0) >= 2000000u) {
            report_stats(window, (uint64_t)(f2 - window_t0), work_sum, emu_sum,
                         present_sum, work_max);
            work_sum = emu_sum = present_sum = work_max = 0;
            window = 0;
            window_t0 = f2;
        }

        ag_yield();
        /*
         * Paced to the console's own frame, 16 683 us (NTSC, 59.94 Hz) - by a
         * deadline that carries over, not by whole milliseconds from this
         * frame's start: that ran at 62.5 frames a second wherever a frame
         * took under 16 ms, which on the CYD is now.  A frame that falls
         * further than two behind moves the deadline up rather than being
         * caught up in a rush.
         */
        next_frame += 16683u;
        const ag_time_t now = ag_micros();
        if ((int64_t)(now - next_frame) > 2 * 16683) {
            next_frame = now;
        } else {
#ifdef ARGON_MD_SMALL
            /*
             * The time left before the next frame goes to the picture, not to
             * sleep.  Lines are drawn while the emulation runs, and since a
             * game's waiting is skipped (m68kcpu.h) a quiet frame is done in
             * 3 ms of its 16.7: the glass got 0.5 pictures a second where it
             * had 7.  A line drawn now shows the VDP as the frame left it,
             * which for every line not yet drawn is what it would have shown.
             */
            while (s_ring_n > 0 && !s_headless && !s_sysdraw &&
                   (int64_t)(next_frame - ag_micros()) > 300) {
                const int line = md_line_begin(-1);
                if (line < 0) {
                    ag_yield();      /* the batch is on its way: let it go */
                    continue;
                }
                gwenesis_vdp_render_line(line);
                md_line_end(line);
            }
#endif
#ifdef ARGON_MD_SMALL
            if (s_sysdraw && s_ring_n > 0 && !s_headless && s_help) {
                md_help_draw(next_frame);
            }
#endif
            const ag_time_t later = ag_micros();
            if (next_frame > later + 1000u) {
                ag_delay((uint32_t)((next_frame - later) / 1000u));
            }
        }
    }

    if (window > 0) {
        report_stats(window, (uint64_t)(ag_micros() - window_t0), work_sum,
                     emu_sum, present_sum, work_max);
    }
    ag_printf("md: %d frames\n", ran);
#ifdef MD_MENU
    md_wait_drawer_idle();
    if (s_to_pick) {
        /* LOAD GAME: everything this cartridge took, back - and the drawer
         * thread gone, so nothing of the game reaches the glass over the
         * city (a frame it drew late stayed on the S3-CAM's top quarter) */
        md_ring_stop();
        s_ring_go = 0;           /* a new thread waits for the next machine */
        s_to_pick = 0;
        s_quit = 0;
        md_state_unkeep();
        md_raster_free(ag_free);
        md_pages_free();
        (void)ag_unmap(rom_data);
        if (ejected) {
            (void)ag_media(true);
            ejected = 0;
        }
        rom = NULL;
        goto md_next_game;
    }
    md_state_unkeep();
    if (ejected) {
        (void)ag_media(true);
    }
#endif
#ifdef ARGON_MD_SMALL
    md_snd_async_stop();
    if (s_ring_n) {
        ag_printf("md: %u rows to the glass, %u refused\n",
                  (unsigned)s_ring_got, (unsigned)s_present_bad);
    }
    md_ring_stop();
#endif
    md_sound_close();
    ag_gfx_release();
    return 0;
}
