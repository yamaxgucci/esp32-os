/*
 * ArgonOS GB - a Game Boy, on a board with 320 KB of memory in total.
 *
 *   gb a:\dmg-acid2.gb          run it until Esc
 *   gb a:\game.gb fps60         draw every frame; the board then runs 8% slow
 *   gb a:\game.gb stats         report rate and where the time goes
 *
 * The emulator is Peanut-GB (MIT, see peanut_gb.h and LICENSE.peanut-gb); this
 * file is the ArgonOS half of it: the cartridge, the screen, the buttons and
 * the clock.
 *
 * Why this fits where a Master System does not
 * -------------------------------------------
 *
 * SMS Plus GX in this tree wants 736 KB of lookup tables, which is more memory
 * than the chip has, and that is not a porting problem - it is the design of a
 * fast emulator for a machine with room.  A Game Boy is a smaller machine and
 * Peanut-GB is written for smaller ones: sixteen and a half kilobytes of state,
 * no tables to speak of, and a renderer that produces one scanline at a time.
 *
 * Everything lives in this file's uninitialised data, one block, no allocator:
 *
 *   cartridge          32 KB   the ROM, whole, because a 32 KB cartridge has
 *                              no bank switching to do
 *   struct gb_s      ~16.5 KB  video memory 8, work memory 8, sprites, ports
 *   band buffer         5 KB   sixteen scanlines on their way to the panel
 *                    -------
 *                     ~54 KB   of the 64 KB largest free block
 *
 * The process arena is not used at all, and that is deliberate: a default arena
 * is capped at a quarter of the pool it comes from, and a named size is refused
 * outright when it cannot be had.  Uninitialised data is one allocation the
 * loader makes before anything runs, and it either loads or it says why.
 *
 * Larger cartridges need the ROM somewhere other than RAM - the flash `appfs`
 * partition, mapped - which is a real next step rather than a hard limit.
 *
 * The screen
 * ----------
 *
 * 160x144 is not the system's surface and never can be here: the surface is
 * shared, sized once at boot, and a Game Boy shaped one would cost 46 KB that
 * the cartridge wants.  So this brings its own pixels (gfx->present, ABI 0.31)
 * and hands them over sixteen scanlines at a time.  Sixteen rather than one
 * because a present costs a window command whatever its height, and 144 of
 * those per frame is most of a frame spent on overhead; nine is nothing.
 *
 * The panel is 320x240, so the picture lands 1:1 in the middle.  Doubling would
 * need 288 rows and there are 240.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>
#include <argon/keys.h>

#include "peanut_gb.h"

/*
 * Eight kilobytes of stack and thirty-six of arena, and both numbers are
 * measured rather than chosen.
 *
 * The default stack is sixteen, which this does not come close to using -
 * Peanut-GB's processor loop is flat and nothing here recurses - and on this
 * board those eight kilobytes are the difference between the arena being
 * findable and not.  The arena holds the cartridge and the band buffer, for the
 * reason given at s_rom.
 *
 * The whole budget, as the board reports it at the moment of loading:
 *
 *   byte-addressable free            62 KB, largest block 56
 *   image data (state + band)        22 KB   uninitialised, one block
 *   arena (the cartridge)            33 KB   asked for after the load
 *
 * A named arena is refused outright when it cannot be had, which is why these
 * are worth being exact about: too large a number here is not a slow program,
 * it is one that does not start.
 */
AG_APP_SIZED("GB", "1.0", "argon", AG_AXE_NEEDS_GFX, 8 * 1024, 33 * 1024);

/* ---- the cartridge ----------------------------------------------------- */

#define ROM_MAX (32u * 1024u)

/*
 * The cartridge is asked for at run time, not declared here, and the reason is
 * arithmetic rather than taste.
 *
 * An image whose code does not fit the IRAM arena is executed from flash
 * instead, and to get it there the loader builds the relocated code in a scratch
 * buffer the size of the code - 22 KB here - writes it, and frees the scratch.
 * That scratch is live at the same time as the image's uninitialised data.  With
 * the cartridge in that data the two came to 80 KB against 76 free, and the load
 * failed before main ever ran.
 *
 * Out here it is 32 KB asked for after the scratch has gone, which fits with
 * room to spare.  The arena is named in AG_APP_SIZED above: a named size is
 * refused outright when it cannot be had, which is the answer wanted - better a
 * clear refusal at load than a cartridge that half fits.
 */
static uint8_t *s_rom;
static uint32_t s_rom_len;

static uint8_t rom_read(struct gb_s *gb, const uint_fast32_t addr)
{
    (void)gb;
    return (addr < s_rom_len) ? s_rom[addr] : 0xffu;
}

/*
 * A cartridge with battery-backed memory in it is refused before it gets here
 * (see main), so these two exist to satisfy the emulator's vtable and to be
 * honest about it rather than to pretend there is memory.
 */
static uint8_t cart_ram_read(struct gb_s *gb, const uint_fast32_t addr)
{
    (void)gb;
    (void)addr;
    return 0xffu;
}

static void cart_ram_write(struct gb_s *gb, const uint_fast32_t addr,
                           const uint8_t val)
{
    (void)gb;
    (void)addr;
    (void)val;
}

static const char *k_gb_error[] = {
    "unknown", "invalid opcode", "invalid read", "invalid write", "halted",
};

static void gb_err(struct gb_s *gb, const enum gb_error_e why,
                   const uint16_t addr)
{
    (void)gb;
    ag_printf("gb: %s at %04x\n",
              (why < 5) ? k_gb_error[why] : "error", (unsigned)addr);
}

/* ---- the screen -------------------------------------------------------- */

#define BAND 8

/*
 * Eight scanlines on their way to the panel.
 *
 * Eight rather than sixteen costs eighteen presents per frame instead of nine -
 * a few hundred microseconds - and buys back three kilobytes.  One at a time
 * would cost a hundred and forty-four presents, and that is where the overhead
 * of a window command per present stops being nothing.
 *
 * In the image rather than the arena, unlike the cartridge: the arena has to be
 * found as a single block after the image is already placed, and every kilobyte
 * asked for there is a kilobyte that has to be contiguous.  Two and a half in
 * the image is cheaper than two and a half in the arena.
 */
static uint16_t s_band[LCD_WIDTH * BAND];

/*
 * The four shades, as the original glass showed them.
 *
 * A Game Boy has no colours - it has one green-grey liquid crystal and two bits
 * per pixel - so any mapping is a choice.  These are the numbers everyone who
 * has seen one recognises, and they matter more than accuracy would: a picture
 * in grey would be correct and would still look wrong.
 *
 * Stored RGB565 in the machine's own byte order, which is what present wants.
 */
static uint16_t rgb565(uint32_t r, uint32_t g, uint32_t b)
{
    return (uint16_t)(((r & 0xf8u) << 8) | ((g & 0xfcu) << 3) | (b >> 3));
}

static uint16_t s_shade[4];

static void palette_init(void)
{
    s_shade[0] = rgb565(0x9b, 0xbc, 0x0f);
    s_shade[1] = rgb565(0x8b, 0xac, 0x0f);
    s_shade[2] = rgb565(0x30, 0x62, 0x30);
    s_shade[3] = rgb565(0x0f, 0x38, 0x0f);
}

static uint32_t s_present_us;
static uint32_t s_frames;

static void band_out(uint16_t y0, uint16_t rows)
{
    const ag_blit_t b = {
        .px = s_band,
        .stride = LCD_WIDTH * sizeof(uint16_t),
        .surf_w = LCD_WIDTH,
        .surf_h = LCD_HEIGHT,
        .x = 0,
        .y = y0,
        .w = LCD_WIDTH,
        .h = rows,
    };
    const ag_time_t t0 = ag_micros();
    (void)ag_gfx_present(&b);
    s_present_us += (uint32_t)(ag_micros() - t0);
}

/*
 * One scanline from the emulator.
 *
 * `pixels` carries the shade in its low two bits; the upper bits say which
 * layer the pixel came from, which a colour Game Boy would need and this does
 * not.  Rows are collected into a band and the band goes out when it is full,
 * or when the last line of the frame lands in it.
 */
static void draw_line(struct gb_s *gb, const uint8_t *pixels,
                      const uint_fast8_t line)
{
    (void)gb;

    const uint16_t row = (uint16_t)(line % BAND);
    uint16_t      *out = &s_band[(size_t)row * LCD_WIDTH];

    for (unsigned x = 0; x < LCD_WIDTH; x++) {
        out[x] = s_shade[pixels[x] & 3u];
    }

    if (row + 1u == BAND || line + 1u == LCD_HEIGHT) {
        band_out((uint16_t)(line - row), (uint16_t)(row + 1u));
    }
}

/* ---- the buttons ------------------------------------------------------- */

/*
 * Two ways in, because this board has neither a keypad nor a keyboard.
 *
 * Keys arrive over the serial console, where there is no key-up event at all -
 * a terminal sends the press and nothing else.  So a key is held for a fixed
 * eighty milliseconds and then let go, which is enough for a menu and enough to
 * walk, and is the same trick the Master System port uses.
 *
 * The stylus is the other way, and it is the one that makes the board playable
 * on its own.  Touch reports console cells, forty by thirty; the picture sits
 * in the middle twenty by eighteen of them, so the columns either side are free
 * for controls: a cross on the left, two buttons on the right, start and select
 * along the bottom.  Nothing is drawn for them - there is nowhere to draw, the
 * margins belong to the console - so they are where a thumb would expect them
 * rather than where a label would be.
 */
#define HOLD_MS 80u

/* The picture, in console cells: 160x144 centred on 320x240, cells of 8. */
#define PIC_COL0 10
#define PIC_ROW0 6
#define PIC_COLS 20
#define PIC_ROWS 18

static uint8_t  s_keys;      /* what the emulator sees, JOYPAD_* inverted */
static uint32_t s_key_until; /* ms after which a console key is let go   */
static uint8_t  s_key_bits;  /* held by the keyboard                     */
static uint8_t  s_tap_bits;  /* held by the stylus                       */

static void keys_apply(void)
{
    /* The joypad register is active low: a set bit is a button *not* pressed. */
    s_keys = (uint8_t)~(s_key_bits | s_tap_bits);
}

static uint8_t key_to_button(uint16_t code)
{
    switch (code) {
    case AG_KEY_UP:    return JOYPAD_UP;
    case AG_KEY_DOWN:  return JOYPAD_DOWN;
    case AG_KEY_LEFT:  return JOYPAD_LEFT;
    case AG_KEY_RIGHT: return JOYPAD_RIGHT;
    case AG_KEY_Z:     return JOYPAD_A;
    case AG_KEY_X:     return JOYPAD_B;
    case AG_KEY_ENTER: return JOYPAD_START;
    case AG_KEY_TAB:   return JOYPAD_SELECT;
    default:           return 0;
    }
}

static uint8_t cell_to_button(int col, int row)
{
    if (col < PIC_COL0) {
        /* Left of the picture: a cross, by thirds of its height. */
        const int third = (row - PIC_ROW0) * 3 / PIC_ROWS;
        if (row < PIC_ROW0) {
            return JOYPAD_UP;
        }
        if (row >= PIC_ROW0 + PIC_ROWS) {
            return JOYPAD_DOWN;
        }
        if (third == 0) {
            return JOYPAD_UP;
        }
        if (third == 2) {
            return JOYPAD_DOWN;
        }
        return (col < PIC_COL0 / 2) ? JOYPAD_LEFT : JOYPAD_RIGHT;
    }
    if (col >= PIC_COL0 + PIC_COLS) {
        /* Right of it: A above, B below. */
        return (row < PIC_ROW0 + PIC_ROWS / 2) ? JOYPAD_A : JOYPAD_B;
    }
    /* Above or below the picture: the two small buttons. */
    if (row < PIC_ROW0) {
        return JOYPAD_SELECT;
    }
    if (row >= PIC_ROW0 + PIC_ROWS) {
        return JOYPAD_START;
    }
    return 0;
}

/* ---- the cartridge on disk --------------------------------------------- */

static int rom_load(const char *path)
{
    ag_stat_t st;
    const ag_err_t serr = ag_stat(path, &st);
    if (serr != AG_OK) {
        ag_printf("gb: %s: %s\n", path, ag_strerror(serr));
        return 1;
    }
    if (st.size < 0x150u) {
        ag_printf("gb: %s is %u bytes - too short to be a cartridge\n", path,
                  (unsigned)st.size);
        return 1;
    }
    if (st.size > ROM_MAX) {
        ag_printf("gb: %s is %u KB; this build holds the whole cartridge in "
                  "memory and stops at %u\n", path,
                  (unsigned)(st.size / 1024u), (unsigned)(ROM_MAX / 1024u));
        return 1;
    }

    s_rom = (uint8_t *)ag_malloc((size_t)st.size);
    if (s_rom == NULL) {
        ag_printf("gb: no room for a %u KB cartridge in the arena\n",
                  (unsigned)(st.size / 1024u));
        return 1;
    }

    const ag_handle_t h = ag_open(path, AG_O_RDONLY);
    if (h < 0) {
        ag_printf("gb: %s: %s\n", path, ag_strerror((ag_err_t)h));
        return 1;
    }

    s_rom_len = 0;
    while (s_rom_len < (uint32_t)st.size) {
        const int32_t n = ag_read(h, s_rom + s_rom_len,
                                  (size_t)st.size - s_rom_len);
        if (n < 0) {
            ag_printf("gb: read: %s\n", ag_strerror((ag_err_t)n));
            (void)ag_close(h);
            return 1;
        }
        if (n == 0) {
            break;
        }
        s_rom_len += (uint32_t)n;
    }
    (void)ag_close(h);

    if (s_rom_len != (uint32_t)st.size) {
        ag_printf("gb: %s: read %u of %u bytes\n", path, (unsigned)s_rom_len,
                  (unsigned)st.size);
        return 1;
    }

    /* Cartridge type 0 is ROM and nothing else, which is what fits. */
    const uint8_t mbc = s_rom[0x147];
    if (mbc != 0x00u) {
        ag_printf("gb: cartridge type %02x needs a bank controller; this build "
                  "runs type 00 (ROM only)\n", (unsigned)mbc);
        return 1;
    }

    char title[17];
    for (int i = 0; i < 16; i++) {
        const uint8_t c = s_rom[0x134 + i];
        title[i] = (c >= 0x20u && c < 0x7fu) ? (char)c : ' ';
    }
    title[16] = '\0';
    ag_printf("gb: %s  %u KB  \"%s\"\n", path, (unsigned)(s_rom_len / 1024u),
              title);
    return 0;
}

/* ---- the machine ------------------------------------------------------- */

/*
 * The emulator's whole state: video memory, work memory, sprites, ports and
 * the processor.  Sixteen and a half kilobytes, in uninitialised data, so the
 * loader either finds room for it before anything runs or refuses to load.
 */
static struct gb_s s_gb;

int ag_main(int argc, char **argv)
{
    bool stats = false;
    /*
     * Every other frame by default, because the board cannot draw sixty and
     * keep time, and of the two it is keeping time that a player feels.  Drawn
     * sixty it manages fifty-five and every game runs eight per cent slow;
     * drawn thirty it runs at exactly its own speed.  `fps60` asks for the other
     * trade.
     */
    bool        skip = true;
    const char *path = NULL;

    for (int i = 1; i < argc; i++) {
        if (argv[i] == NULL) {
            continue;
        }
        if (argv[i][0] == 's' || argv[i][0] == 'S') {
            stats = true;
        } else if (argv[i][0] == 'f' || argv[i][0] == 'F') {
            /* fps30 / fps60: how often to draw, not how fast to run. */
            skip = (argv[i][3] != '6');
        } else if (path == NULL) {
            path = argv[i];
        }
    }
    if (path == NULL) {
        ag_printf("usage: gb <cartridge.gb> [fps30|fps60] [stats]\n");
        return 1;
    }

    if (ag_api()->gfx == NULL || !AG_HAS(ag_api()->gfx, present)) {
        ag_printf("gb: this kernel cannot show an application's own pixels "
                  "(needs ABI 0.31)\n");
        return 1;
    }

    if (rom_load(path) != 0) {
        return 1;
    }

    const enum gb_init_error_e err =
        gb_init(&s_gb, rom_read, cart_ram_read, cart_ram_write, gb_err, NULL);
    if (err != GB_INIT_NO_ERROR) {
        ag_printf("gb: init failed (%d)\n", (int)err);
        return 1;
    }
    palette_init();
    gb_init_lcd(&s_gb, draw_line);

    ag_gfxinfo_t info;
    const ag_err_t gerr = ag_gfx_acquire(&info);
    if (gerr != AG_OK) {
        ag_printf("gb: display: %s\n", ag_strerror(gerr));
        return 1;
    }
    /*
     * Drawing every other frame, when asked.
     *
     * The emulator runs every frame either way - a Game Boy game keeps its own
     * time and skipping emulation would slow the game down, which is the thing
     * being avoided.  What is skipped is the *rendering*, and with it the twelve
     * milliseconds of panel: at sixty frames drawn the board manages fifty-five,
     * so the game runs eight per cent slow, and at thirty drawn there is room to
     * spare and it runs at exactly its own speed.
     *
     * Thirty updates a second on a screen is not a compromise anybody notices;
     * a game running slow is.
     */
    s_gb.direct.frame_skip = skip;

    ag_printf("gb: %ux%u panel, picture 160x144 centred, drawing every %s "
              "frame; Esc quits\n", (unsigned)info.width,
              (unsigned)info.height, skip ? "other" : "");

    keys_apply();

    /*
     * One NTSC-ish Game Boy frame is 16.74 ms.  The pacing is against the clock
     * rather than a frame counter so that a slow frame is not paid for twice.
     */
    const uint32_t frame_us = 16743u;
    uint64_t       next = ag_micros();
    uint32_t       stat_at = ag_millis();
    uint32_t       stat_frames = 0;
    uint32_t       emu_us = 0;
    bool           running = true;

    while (running) {
        if (ag_interrupted()) {
            break;
        }

        ag_event_t ev;
        while (ag_poll_event(&ev, 0)) {
            if (ev.type == AG_EV_QUIT) {
                running = false;
                break;
            }
            if (ev.type == AG_EV_KEY_DOWN) {
                if (ev.key.keycode == AG_KEY_ESC) {
                    running = false;
                    break;
                }
                const uint8_t b = key_to_button(ev.key.keycode);
                if (b != 0) {
                    s_key_bits = b;
                    s_key_until = ag_millis() + HOLD_MS;
                }
            } else if (ev.type == AG_EV_POINTER_DOWN ||
                       ev.type == AG_EV_POINTER_MOVE) {
                s_tap_bits = cell_to_button(ev.ptr.x, ev.ptr.y);
            } else if (ev.type == AG_EV_POINTER_UP) {
                s_tap_bits = 0;
            }
        }
        if (s_key_bits != 0 && ag_millis() >= s_key_until) {
            s_key_bits = 0;
        }
        keys_apply();
        s_gb.direct.joypad = s_keys;

        const ag_time_t t0 = ag_micros();
        gb_run_frame(&s_gb);
        emu_us += (uint32_t)(ag_micros() - t0);
        s_frames++;
        stat_frames++;

        if (stats && ag_millis() - stat_at >= 2000u) {
            const uint32_t ms = ag_millis() - stat_at;
            const uint32_t n = stat_frames ? stat_frames : 1u;
            /*
             * Per cent of real time, which is the only figure a player feels:
             * a hundred means the game is running at its own speed, whatever
             * the screen is doing.
             */
            const uint32_t realtime = (uint32_t)(((uint64_t)stat_frames *
                                                  frame_us) / (ms * 10u));
            ag_printf("gb: %u fps, %u%% of real time, emu %u us, show %u us "
                      "per frame\n",
                      (unsigned)(stat_frames * 1000u / ms),
                      (unsigned)realtime, (unsigned)(emu_us / n),
                      (unsigned)(s_present_us / n));
            stat_at = ag_millis();
            stat_frames = 0;
            emu_us = 0;
            s_present_us = 0;
        }

        /*
         * Wait out the rest of the frame.
         *
         * Whole milliseconds, and no spinning for the remainder - which was
         * tried, on the reasoning that a frame is also a clock and a two per
         * cent error in it will be audible once there is sound.  Measured, it
         * was worse: fifty-seven frames a second against fifty-eight, because
         * the spin keeps the processor from the tasks that share it and they
         * take their time back afterwards.  Left as it is until there is sound
         * to say otherwise.
         *
         * The target accumulates rather than restarting from now, so a frame
         * that ran long is paid for by the next sleep being shorter, and the
         * average holds.  Falling behind resets it: catching up by running two
         * frames back to back turns a slow board into a jerky one.
         */
        next += frame_us;
        const uint64_t now = ag_micros();
        if (next > now) {
            const uint32_t wait = (uint32_t)(next - now);
            if (wait > 1000u) {
                ag_delay(wait / 1000u);
            }
        } else {
            next = now;
        }
    }

    ag_gfx_release();
    ag_printf("gb: %u frames\n", (unsigned)s_frames);
    return 0;
}
