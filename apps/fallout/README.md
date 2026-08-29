# Fallout (`FALLOUT.AXE`, `FALLOUT2.AXE`)

[fallout1-ce](https://github.com/alexbatalov/fallout1-ce) and
[fallout2-ce](https://github.com/alexbatalov/fallout2-ce) — Alexander Batalov's
reverse-engineered Fallout 1 and 2 — as ArgonOS `.AXE` images. Compiled for the
ESP32-S3, running from flash XIP with the data in PSRAM.

**Fallout 2 reaches its main menu on an ESP32-S3, and sits there in its attract
loop.** Both opening movies play - 2502 frames of `intro.mve` at 640×480, none
dropped - then the menu comes up with its music, waits, times out, and plays
the intro again, exactly as the game does on a desk. The 326 MB `MASTER.DAT` is
opened off a FAT32 card, its 23120-entry index parsed, the art and protos and
scripts loaded, the audio device opened at 22 kHz.

What has not been done is play it: that needs input through the menu, and
`KBDVIRT`/`MOUSEVIRT` are not wired into this port yet.

| | Fallout 1 | Fallout 2 |
|---|---|---|
| translation units compiled | 108 of 108 | 126 of 126 |
| code, after `--gc-sections` | 453 KB | 761 KB |
| data (mostly bss) | 1.82 MB | 1.91 MB |
| extended memory asked for | 5.75 MB | 12 MB on a 16 MB part |
| game data on the card | — | 562 MB |

**Where it is.** The engine runs the whole of `game_init` and stops where the
art data is not there:

```
SDL: display 640x480 with a framebuffer, 1280 bytes per row
Fade time is 956
Fade steps are 43
Chi squared is 18.75, P = 4000.00 at 0.05
Sequence is random, 95% confidence.
SDL: audio 22050 Hz, 2 channels, blocks of 1024 frames
>gsound_init  >initMovie  >gmovie_init  >moviefx_init
cache_init failed in art_init
art_init failed in iso_init
Failed on iso_init
```

The window is opened, the display is acquired, the palette fade runs sixty
frames at about 57 fps, the engine's own random number generator passes its
chi-squared self-test, the audio device opens and the mixer thread starts.
`iso_init` is where the missing art stops it.

The shim is finished: video, input, audio, timers. What it has not been able
to do is draw anything, because there is nothing to draw. **The next thing this
needs is a copy of the game** - see **The data** below. Everything the shim can
be shown to do without one, `SDLTEST.AXE` shows.

## Licensing

| Part | Licence | Where |
|---|---|---|
| `port/`, `build.py`, `fallout_main.cc` | Apache-2.0 | here |
| fallout1-ce | **Sustainable Use License 1.0** — non-commercial, not an open licence | fetched |
| adecode, fpattern | MIT | fetched |
| MASTER.DAT and the rest | proprietary | yours |

**The engine is not in this repository and must not be put in it.** Its licence
forbids commercial use, which is not a condition the rest of this tree is under.
`build.py` clones it into `third_party/`, where `.gitignore` keeps it — the same
arrangement `apps/gfxbench` uses for LVGL, and the same reason `apps/doom` keeps
`doom1.wad` out.

The game data is nobody's to distribute either. Supply your own from a copy of
the game.

## Build

Needs `xtensa-esp32s3-elf-gcc` on PATH (IDF export).

```
python apps/fallout/build.py
```

First run clones the engine and its two dependencies (about 40 MB), then
compiles 118 objects and links `FALLOUT.AXE` into `build/apps/`.

```
python apps/fallout/build.py --survey
```

Compiles every translation unit and reports what they weigh without linking.
Useful when the shim is in flux, and it is how the numbers below were measured.

## Run

The image is 350 KB in the file and cannot come over HostFS — put it on the SD
image.

```
argon sync build\sd_card
```

```
argon run -Sd
```

Then, in the guest:

```
run a:\fallout.axe
```

The game data goes on the same card, next to the image: `MASTER.DAT`,
`CRITTER.DAT` and the `DATA` folder, exactly as the game installs them.

## What it costs

Measured, not estimated. `--survey` for the first four, the linker for the rest.

| | |
|---|---|
| translation units that compile unmodified | **108 of 108** |
| code before the linker looks | 560 KB |
| code in the image, after `--gc-sections` | **439 KB** |
| data + bss | **1.73 MB** (287 KB stored, 1.45 MB zeroed) |
| relocations | 5109 |
| stack actually used, through `game_init` | 4.8 KB of 16 KB |
| frame rate of the engine's own palette fade, in QEMU | ~57 fps |

439 KB of code does not fit the 192 KB arena, so the loader puts it in flash and
executes it in place. That is automatic — the image asks for nothing. The data
goes to PSRAM, where 8 MB leaves room for the art cache the game has yet to
allocate.

The engine source is **unmodified**. Not one patch, not one `#ifdef ARGON`.
Everything that had to give way gave way on this side: `port/SDL.h` is the SDL2
subset the engine actually uses, `port/dirent.h` shadows the toolchain's so that
`d_type` exists, and the rest is in `port/`.

## What the shim does

All of it is in `port/`, and `SDLTEST.AXE` is the test - it makes the same
calls `svga.cc` makes, in the same order, then reads the framebuffer back and
checks it by arithmetic rather than by eye.

**Video, without a copy per frame.** The engine asks its texture what format it
came out as and builds its presenting surface to match. Answering RGB565 - not
the 32-bit it asked for - means that surface can *be* the framebuffer: the
game's 8-bit blit through the palette lands directly where the panel reads, and
`SDL_RenderPresent` is a flush and nothing else. The palette is turned into a
table of RGB565 once per blit, 256 conversions instead of 307200.

The game is 640×480 and the default framebuffer is 640×400 (80×25 text cells).
Put this in `C:\SYSTEM.CFG` and reboot, or the picture is copied and cropped
instead:

```ini
[display]
width  = 640
height = 480
```

**Input.** ArgonOS reports a key as a USB HID usage id and an SDL scancode *is*
a USB HID usage id, so the translation is nearly the identity. The mouse needs
more care: the engine wants relative movement, MOUSEVIRT sends absolute
positions, and the difference is taken in the shim. ArgonOS numbers the buttons
left/right/middle and SDL numbers them left/middle/right - two bits change
places, and getting that wrong loses the right button, which is how Fallout
switches cursor mode.

**Audio.** SDL calls back for samples; ArgonOS is written to. So the callback is
driven from a thread that asks `ag_audio_space` for room, calls the engine's
mixer, and writes. `SDL_AudioStream` is a linear-interpolating resampler -
every voice goes through one, because the game's sounds are recorded at
whatever rate they were recorded at and mixed at 22 kHz.

**Timers** are serviced from `SDL_RenderPresent`, the one point the game passes
through every frame. The engine registers one: the 40 ms sound fade. It
therefore advances at the frame rate, which on a slower machine is a slower
fade; the alternative is a fourth thread for something nobody hears.

## Running it

The game data is nobody's to distribute: supply your own. `MASTER.DAT`,
`CRITTER.DAT` and the `DATA` folder go at the root of the card, beside the
`.AXE` and `fallout2.cfg`.

```
python apps/fallout/build.py --game 2 --arena-kb 12000
```

```
argon sync build\fo2_card -SizeMB 1024 -Image build\fo2card.img
```

```
argon test -Sd -SdImage build\fo2card.img -Psram 16 "run a:\fallout2.axe"
```

The game gets a card image of its own rather than the default
`build\sdcard.img`. Not tidiness: the game runs for minutes, `argon sync` for
anything else rewrites that file underneath the emulator, and the run then does
nothing at all with no indication why. Half an hour went into a run that had
had its card replaced mid-flight.

`-Image` rather than `-Out`, which PowerShell binds to its own common
parameters before the script sees it.

Three things there are not decoration:

**`-SizeMB 1024`.** The game is 562 MB and a FAT16 image stops at 512, so this
is a FAT32 card - `mkfatimg` picks the format by size. QEMU's SD model wants a
power of two, so 700 MiB silently produces a machine that will not start.

**`-Psram 16`.** The default emulated part has 8 MB, and Fallout 2 does not fit
in it: `wmConfigInit` runs out while loading the encounter tables. 16 MB is the
other size a real S3 module comes in, and 16 + 8 of flash still fits the 32 MB
data-bus window.

**`--arena-kb 12000`.** The process asks for its extended memory rather than
taking the default quarter-of-what-is-free. See the comment on `AG_APP_SIZED`
in `fallout_main.cc` - the two `.DAT` indices alone are 1.7 MB.

`fallout2.cfg` matters too, for one line: `art_cache_size=1`. The stock value
is 8, in megabytes, reserved whole at startup.

## What a frame will cost on a board

QEMU cannot answer this and neither can `-Icount`: the emulator models neither
the cache nor PSRAM latency, so an instruction count is a floor and not an
estimate. `SDLTEST.AXE` prints the three numbers that matter and is the thing
to run on a board when there is one. In QEMU it prints, for the record:

```
frame cost, 640x480, 600 KB per full screen:
  blit 8-bit through palette :   4817 us   (CPU + PSRAM)
  flush whole screen         :     48 us   (panel bus)
  flush 280x80 rectangle     :   5079 us   (43 KB)
```

The blit is a floor - a real part pays cache misses and PSRAM latency that
QEMU does not model. The flushes are not about pixels at all: the soft display
shows the picture through a doorbell, a full-screen kick may replace one
already in flight and returns immediately, and a smaller one has to wait for
the window or it would leave stale rows. That is why the small rectangle is
the slower of the two here and will be the faster of the two on a panel.

What follows is arithmetic from the measured figures in
[`docs/07-emulator-performance.md`](../../docs/07-emulator-performance.md).

A full 640×480 screen is 614 KB in RGB565.

**Our part, the blit.** The engine draws into an 8-bit surface; the port turns
that into RGB565 through the palette. Full screen, that reads 307 KB and
writes 614 KB - about 0.9 MB of PSRAM traffic, and at the 40-80 MB/s that
octal PSRAM does on a copy, **12-23 ms**.

**The panel.** On SPI at 80 MHz, 614 KB is about **60 ms** of bus time - the
same document measures 14 ms for a 143 KB frame, and it scales. That is a
ceiling of 16 frames a second before anything else happens. On an RGB panel,
which scans the framebuffer itself, there is no transfer at all.

Both costs are proportional to the area that changed, not to the screen:
`GNW95_ShowRect` blits only what a window touched, and `SDL_RenderPresent`
sends only the union of what was blitted since the last one. So:

- **dialogue, inventory, the character sheet, turn-based combat** - small
  rectangles, and the cost is a fraction of the above;
- **scrolling the world** - the whole screen, every frame. Single digits on
  SPI; on an RGB panel it is the 12-23 ms blit alone, so 40-80.

Which says the board this wants is one with an RGB panel, and that the CYD -
320×240 over SPI - is the wrong shape for it twice over.

## Where the memory goes, and why 8 MB is not enough

`-memstat N` makes the port print the arena's use and its high-water mark every
N milliseconds. Through `gameInit`, both movies and the main menu:

```
[mem] 6770 KB used of 12000   during gameInit
[mem] 7312 KB used of 12000   sitting in the main menu
[mem] 8043 KB used of 12000   peak, replaying the intro
```

Still a floor: no map has been loaded. Against it:

| | |
|---|---|
| the image's own data, almost all bss | 1867 KB |
| peak the game reached | 8043 KB |
| **needed** | **9910 KB** |
| free on a part with 8 MB of PSRAM | 8040 KB |

Short by about 1900 KB - and by 2500 on a board, because QEMU lends the
emulated panel's own memory for the framebuffer and a real 640×480 panel takes
its 600 KB out of the same PSRAM. On a 16 MB part it is 9910 of 16384, with
room for a map.

Inside the arena:

| | |
|---|---|
| `MASTER.DAT` index, 23120 entries | ~1.3 MB |
| `CRITTER.DAT` index, 7120 entries | ~0.4 MB |
| art cache (`art_cache_size`, minimum 1) | 1 MB |
| sound cache (`cache_size`) | 256 KB |
| 640×480 screen buffer, and the 8-bit surface | 600 KB |

And the largest fixed arrays in the image, which is what "optimise the engine"
would have to mean:

| bytes | |
|---|---|
| 480 000 | `gTileIntensity` - `int[3][40000]`, three map levels of 200×200 hexes |
| 196 608 | three 256×256 colour tables (intensity, mix-add, mix-multiply) |
| 162 752 | animation sequences and their state |
| 160 000 | `gObjectListHeadByTile` - one pointer per hex |
| 120 000 | `_square_data` |
|  80 000 | two path-finding node lists |
|  80 000 | `_script_dialog_msgs` |

The plausible savings, with what each costs to get:

- **192 KB, free**: `cache_size=64` in the `.cfg` instead of 256.
- **240 KB, one line of engine**: `gTileIntensity` as `uint16_t`. Light is
  clamped to 65536 on read and 65536 is one past what fits, so it would have to
  saturate at 65535 - a difference nobody can see, in a vendored file we would
  then have to patch on every update.
- **250-370 KB, one function of engine**: `dbaseOpen` allocates a separate
  block for each of the 30240 entry paths, so a quarter of that memory is
  allocator headers. One block for all of them would recover it.

About 800 KB in total, against a gap of 2500. It does not close, it is not
close to closing, and every metre of it is paid for in patches against somebody
else's engine. **A 16 MB module is the answer**; `argon test -Psram 16` is the
same choice in QEMU.

Each `.DAT` entry is a descriptor plus a separately allocated path, and the
index is read entry by entry - about 150000 small reads for the two of them,
which is most of the ten seconds before the first frame.

Running out does not announce itself in the engine: `dbaseOpen` stops reading
entries at the first failed allocation and hands back the half-built table
anyway, and the binary search over it then cannot find files that are
demonstrably in the archive. `apps/common/libc` now prints the first failed
`malloc` with the arena's state, which is what turned that into a one-line
answer instead of an afternoon.

## Notes for whoever picks this up

- `ARGON_FALLOUT_LDFLAGS` is passed to the link. `-Wl,-Map=<path>` and
  `-Wl,-y,<symbol>` are how the archive-inclusion chains below were untangled;
  neither path may contain a space, because mkaxe splits ldflags on them.
- One call to `localtime` used to pull in the whole of newlib's stdio - through
  `tzset`, `siscanf`, `svfiscanf`, `ungetc`, `fflush`, the FILE table - and four
  of those collided with `apps/common/libc`. `port/posix_io.cc` implements the
  calendar instead. If a new engine call drags an archive member in, the map
  file names the symbol that did it.
- The task stack must come from internal SRAM. Asking for 32 KB when 31 KB was
  free in one piece put the stack in PSRAM, and the process faulted before its
  first instruction. `AG_APP` rather than `AG_APP_SIZED` until there is a
  measurement saying otherwise.
- `build.py` compiles the engine with `-D_DEBUG`. That is not about assertions:
  `debug_printf` is compiled out without it, and the engine then stops halfway
  in silence. An afternoon went into finding out that the silence was the build
  flag rather than a fault.
- `%.4s` is a precision, and the shim's `snprintf` used to parse it and throw it
  away. Fallout builds the name of a message file that way — `pro_%.4s.msg` of
  `"items"` — so the engine asked the archive for `pro_items.msg`, was told
  there was no such file, and reported that it could not load its prototypes.
  Fixed in `apps/common/libc`, where it had been wrong for every port.
- Fallout 2 uses `std::ofstream` in `display_monitor.cc` and `std::random_device`
  in `sfall_arrays.cc`. Either one pulls in the whole of libstdc++'s iostreams
  and locale, which then calls newlib's stdio and its string-to-number
  conversions — the same names the libc shim defines. `build.py` links, sees
  which symbols collide, checks that ours were defined first, and only then
  retries with `--allow-multiple-definition`. A collision between two of our
  own files still fails, which is the point.
- Three engine files use `getenv`, `pow` and `sqrtf` without including the
  header that declares them. That compiles against glibc and not against
  newlib. `port/engine_prelude.h` is forced in front of every translation unit
  with `-include`; the engine is vendored and is not ours to edit.
