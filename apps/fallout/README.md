# Fallout (`FALLOUT.AXE`)

[fallout1-ce](https://github.com/alexbatalov/fallout1-ce) — Alexander Batalov's
reverse-engineered Fallout 1 — as an ArgonOS `.AXE`. 128k lines of C++ compiled
for the ESP32-S3, running from flash XIP with 1.8 MB of data in PSRAM.

**Where it is.** The engine reaches its own `main`, initialises, looks for the
game data and says so when it is not there:

```
FALLOUT - fallout1-ce on ArgonOS
message: Could not find the master datafile. Please make sure the FALLOUT CD is
in the drive and that you are running FALLOUT from the directory you installed it to.
```

Everything up to that point works. Nothing after it does yet: the SDL shim
still panics on `SDL_CreateWindow`, so there is no picture, no input and no
sound. See **What is left** below.

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
| stack actually used, up to the data check | 4.6 KB of 16 KB |

439 KB of code does not fit the 192 KB arena, so the loader puts it in flash and
executes it in place. That is automatic — the image asks for nothing. The data
goes to PSRAM, where 8 MB leaves room for the art cache the game has yet to
allocate.

The engine source is **unmodified**. Not one patch, not one `#ifdef ARGON`.
Everything that had to give way gave way on this side: `port/SDL.h` is the SDL2
subset the engine actually uses, `port/dirent.h` shadows the toolchain's so that
`d_type` exists, and the rest is in `port/`.

## What is left

In the order it would have to be done.

**Video.** `SDL_CreateWindow` and `SDL_RenderPresent` panic. The engine draws
640×480 8-bit into a surface and blits it through a palette into a 32-bit one -
that blit is written and works. What is missing is the last step: 32-bit surface
to the ArgonOS framebuffer. `fb0` is RGB565 and 640×400 by default, so 480 rows
need either a taller framebuffer or a viewport.

**Input.** `SDL_PollEvent` returns nothing. Fallout is a mouse game end to end;
`/dev/mouse0` and the keyboard both exist and the scancode table in `port/SDL.h`
is already the shape `kb.cc` wants.

**Audio.** `SDL_OpenAudioDevice` reports success and is silent. The engine mixes
its own voices and hands over one stream, so the work is one PCM sink plus a
resampler for `SDL_AudioStream` - the sample rates in the game's own files vary
per sound.

**Then the data.** 400 MB of `.DAT` over SPI, seeked rather than streamed. That
is where the real performance question lives, and nothing measured so far says
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
