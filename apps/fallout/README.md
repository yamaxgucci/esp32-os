# Fallout (`FALLOUT.AXE`, `FALLOUT2.AXE`)

[fallout1-ce](https://github.com/alexbatalov/fallout1-ce) and
[fallout2-ce](https://github.com/alexbatalov/fallout2-ce) — Alexander Batalov's
reverse-engineered Fallout 1 and 2 — as ArgonOS `.AXE` images. Compiled for the
ESP32-S3, running from flash XIP with the data in PSRAM.

**Fallout 2 plays its opening cinematic on an ESP32-S3.** 2502 frames of
`intro.mve` at 640×480, none dropped. Everything before it works: the 326 MB
`MASTER.DAT` is opened off a FAT32 card, its 23120-entry index parsed, the art
and protos and scripts loaded, the audio device opened.

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

## Where the memory goes

| | |
|---|---|
| `MASTER.DAT` index, 23120 entries | ~1.3 MB |
| `CRITTER.DAT` index, 7120 entries | ~0.4 MB |
| art cache (`art_cache_size`) | 1 MB |
| 640×480 screen buffer | 300 KB |

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
