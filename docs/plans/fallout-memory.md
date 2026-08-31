# Fallout 2 in 8 MB of PSRAM: what is left to do

Written 31 August 2026, branch `worktree-fallout-cxx`. Everything here is
measured; the numbers are the point, and the READMEs they came from are
`apps/fallout/README.md` under **What it would take to have room**.

## Where it stands

Shipping configuration — `-Psram 8`, arena 6100 KB (`apps/fallout/build.py`,
`ARENA_KB`), art cache 1400 KB, music ring 4×16 KB:

| | |
|---|---|
| frame rate standing on the Temple of Trials | 27 fps, median of 210 windows |
| arena high-water, transients included | 5542 KB of 6100 |
| art cache, standing still | reads nothing, evicts nothing |
| a map change wants, on top of standing still | 837 KB |
| **short by** | **184 KB** on the arithmetic; the failure itself comes at 33 KB free |

**The arena cannot be raised.** 8007 KB of extended memory is free with the
card mounted and the image's data takes 1812, but the arena must be one block:
6150, 6220 and 6360 are all refused. Do not spend runs on this again.

**The framebuffer is free in QEMU and will not be on a board.** The panel's
640×480 RGB565 buffer is 600 KB and comes from the emulated device
(`esp_lcd_rgb_qemu_get_frame_buffer` in `components/argon_port/idf/src/panel_hw.c`).
On hardware it is PSRAM, so every figure above is 600 KB optimistic there.

## How to measure anything

Two runs, always, because `argon test` builds the firmware first and that
deletes `C:\SYSTEM.CFG`, which is worth 608 KB of arena:

```bash
argon fallout --game 2 --rodata-in-code
cp build/apps/FALLOUT2.AXE build/fo2_card/ && cp apps/fallout/fallout2.cfg build/fo2_card/
argon sync build\fo2_card -SizeMB 1024 -Image build\fo2card.img
argon test -Sd -SdImage build\fo2card.img -TimeoutSec 150 -LogPath build\prime.log "copy a:\system.cfg c:\system.cfg"
argon test -Sd -SdImage build\fo2card.img -Psram 8 -TimeoutSec 700 -LogPath build\run.log "run a:\fallout2.axe -autostart -fps 3000 -memstat 30000"
```

`[mem] … peak N (M with transients)` is the number that matters; `M` is what
the part has to satisfy. Check `SDL: display 640x480` in the log before
believing anything — 640x400 means the prime run did not take.

A map change needs the mouse: install `MOUSEVIRT.SYS`, wait for `HMWARRAA` in
the log (the map is up), then click the temple doorway at about (312, 90) in
guest pixels with `tools/inputplay.py`.

## The work, largest first

### 1. `gTileIntensity` — 320 KB, and 320 more

`third_party/fallout2-ce/src/light.cc:20`:

```cpp
static int gTileIntensity[ELEVATION_COUNT][HEX_GRID_SIZE];   // int[3][40000] = 480 KB of bss
```

Six accesses, all in `light.cc` (lines ~74, 88, 102, 116, 130, 138).

**(a) Per elevation, 320 KB.** Almost every map has one of the three
elevations. Make it `static int* gTileIntensity[ELEVATION_COUNT]` and allocate
a row on first touch, or at map load for the elevations
`elevationIsValid()` reports. 480 KB of bss becomes 160 KB of arena on a
one-level map. Allocate at load rather than lazily if a failed allocation
mid-frame would be awkward to handle.

**(b) Narrower, another 240 KB on top.** The values are added and subtracted
(`+=`, `-=`) and pass `LIGHT_INTENSITY_MAX` (65536) before being clamped on
read, so they do not fit `uint16` as they are. Stored shifted down two bits
they do: `int16` holds 32767, so 131068 after the shift, which is two whole
lights on one hex. Two bits of a value that ends as one of sixty-four palette
shades is not visible. Three lights overlapping one hex would saturate, and
removing one of them would then leave the hex too dark — decide whether that
matters before doing this half.

Do (a) first and measure. It alone covers the 184 KB.

### 2. The four zlib windows — 128 KB

Four 32 KB inflate windows were live at once in the survey, one per open
archive stream (`zcalloc` ← `updatewindow`). Patch 0005 keeps the archive file
open; what has not been looked at is how many `DFile` streams are open
simultaneously and whether that number can be smaller. `inflateEnd` frees the
window, so this is about lifetime, not about `windowBits`.

### 3. `_square_data` — 80 KB

Same three-levels-for-one trick as `gTileIntensity`, in `map.cc`
(`square_init`, `_square_reset`, `_square_load`).

### 4. The panel at 320×240 — 450 KB on a board, and needed anyway

The CYD is 320×240, so this has to happen regardless of memory. The engine
keeps drawing 640×480 into its own 8-bit surface; the port's blit
(`SDL_UpperBlit` in `apps/fallout/port/sdl_shim.cc`) takes every second pixel
and every second row into a 320×240 panel. Text will be unreadable — Fallout's
interface is bitmaps laid out for 640×480 — so this is about running at all,
not about playing comfortably.

Watch the dirty rectangle: `dirty_add` is in panel coordinates, so halve it
there rather than in two places.

## Already done, do not redo

| | |
|---|---|
| music ring 24×32 KB → 4×16 KB | patch 0003, −704 KB from stock |
| `.DAT` index mapped from flash | patch 0001, −1841 KB |
| the map's ambience streamed, not decoded whole | patch 0004, −488 KB |
| art cache sized in kilobytes, set to 1400 | patches 0012, `fallout2.cfg` |
| `_GNW_win_refresh` fills the destination instead of a copy | patch 0011, −307 KB of transient, and it is what left black rectangles under the cursor |

Ruled out, with the reason:

- **`_screen_buffer`** looked like 300 KB of dead allocation and is dead, but
  was never allocated: the flag `gameInitWithOptions` passes is zero.
- **A bigger arena.** See above.
- **2 MB of art cache.** 1400 KB measures identically.
- **Flushing the art cache before a map loads.** It sounds right - the old
  map's tiles are dead weight and `artCacheFlush` exists and is never called
  outside the death screen - and it changes nothing: with the flush as the first
  statement of `mapLoad`, the failure comes at 33 KB free of 6100 instead of 41,
  which is the same number twice. Whatever the cache is holding at that moment
  is still referenced, so `cacheFlush` keeps it. Tried at both ends of
  `mapLoad`; do not try a third time without first finding out *what* is still
  locked.

## The 837 KB is most likely not transient at all

Entering the Temple of Trials is not a map change.  In Fallout 2 the temple is
one map, `ARTEMPLE.MAP`, with more than one elevation, and going inside changes
elevation - the log carries one `OVERRIDE_MAP_START` for the whole run and never
names a second map.  The failing allocation is 32 KB for `water1.ACM`, a sound
of the level being entered.

Which means the 837 KB is not a spike to be smoothed out.  The new elevation's
objects, tiles and sounds are brought in and the previous elevation's are *not*
released, because it is the same map and the player can walk back up.  It is the
map's resident footprint growing as its levels are visited.

Two things follow.  Freeing before loading has nothing to free, which is why
flushing the art cache did nothing.  And the only lever is the baseline: every
kilobyte taken out of bss or out of a cache is a kilobyte the second elevation
can have.  The order of work below does not change; the expectation of what
finishes the job does - 320 KB may not be enough, and how much is needed will
only be known once a second elevation has actually been stood on.

The cheap way to find out, and it is one run: sfall's `StartingMap` is read in
`main.cc` right where `_main_load_new` is called, so putting the temple's inner
level in `ddraw.ini` under `[Misc] StartingMap=` and starting with `-autostart`
lands on it directly.  `[mem]` then gives its resident cost with nothing else
resident, and the difference from 5542 KB is the real answer.

## What else is not known about it

It is inferred, not measured: standing still peaks at 5542 KB and the failure
comes at about 6067 KB used, so a map change wants roughly that much more. What
it is spent on has not been broken down. `-memstat` cannot see it because the
arena watcher only samples on allocations of 64 KB and up, and the allocation
that fails is 32 KB.

The way to find out: lower `ARENA_WATCH_MIN` in `apps/common/libc/libc_shim.c`
for one run, keep the three-frame `report_big` trace, and walk into the temple.
That gives every allocation over the threshold with the function that asked for
it, across the transition. Until that is done, the 320 KB from `gTileIntensity`
is a guess at whether it is enough.
