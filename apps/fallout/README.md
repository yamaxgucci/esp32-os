# Fallout (`FALLOUT.AXE`)

[fallout1-ce](https://github.com/alexbatalov/fallout1-ce) — Alexander Batalov's
reverse-engineered Fallout 1 — as an ArgonOS `.AXE`. 128k lines of C++ compiled
for the ESP32-S3, running from flash XIP with 1.8 MB of data in PSRAM.

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

## The data

`MASTER.DAT`, `CRITTER.DAT` and the `DATA` folder from a copy of the game, at
the root of the card next to `FALLOUT.AXE`. Nobody else's to distribute, and
the engine looks for them relative to where it was started.

Until they are there, `apps/fallout/testdata` gets the engine through
`game_init` on a database with nothing in it - see its README. That is how
everything above was tested.

Once they are: 400 MB of `.DAT` over SPI, seeked rather than streamed. That is
where the real performance question lives, and nothing measured so far says
anything about it.

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
