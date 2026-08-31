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
| arena high-water on the temple, transients included | 5542 KB of 6100 |
| the same on the worst map in the game | 5843 KB of 6100 |
| art cache, standing still | reads nothing, evicts nothing |
| **short by** | **215 KB**, and only during a change of map |

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

### 1. `gTileIntensity` — 240 KB, on every map (and 320 more only on one-level maps)

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

**Do (b), not (a).**  The measurements above turned this round: (a) buys
nothing on a three-elevation map, and a three-elevation map is the case that
fails.  (b)'s 240 KB is on every map and covers the 215 KB on its own.

### 2. The four zlib windows — 128 KB

Four 32 KB inflate windows were live at once in the survey, one per open
archive stream (`zcalloc` ← `updatewindow`). Patch 0005 keeps the archive file
open; what has not been looked at is how many `DFile` streams are open
simultaneously and whether that number can be smaller. `inflateEnd` frees the
window, so this is about lifetime, not about `windowBits`.

### 3. `_square_data` — 80 KB

Same three-levels-for-one trick as `gTileIntensity`, in `map.cc`
(`square_init`, `_square_reset`, `_square_load`), and worth the same caveat:
it pays on the 109 one-elevation maps and not on the twenty that need it.

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

## What a map actually costs, measured

Three maps, each started directly with sfall's `StartingMap` in `ddraw.ini`
and `-autostart`, so each is weighed with nothing else ever resident.  16 MB of
PSRAM and a 10000 KB arena for the runs, so nothing fails and the numbers are
the demand rather than what happened to fit:

| map | file in `master.dat` | used, standing still | peak | with transients |
|---|---|---|---|---|
| `ARTEMPLE.MAP` | 93 KB | 5123 KB | 5502 | 5542 |
| `ARVILLAG.MAP` | 216 KB | 5605 KB | 5639 | 5843 |
| `BROKEN2.map` | 767 KB, the largest in the game | 5605 KB | 5639 | 5843 |

Every map's header says which of the three elevations it actually holds - a
*set* bit in `flags` means the level is **absent** (`_map_data_elev_flags` in
`map.cc`, and the tile reads at 1417 and 1727 which run when the bit is clear).
Decoded for all 155 maps in `master.dat` and `critter.dat`: **109 have one
elevation, 26 have two, 20 have three.**  Broken Hills is the largest file in
the game and one of the twenty.

So of the three measured maps, two are one-elevation and one is three:

| map | elevations | file | used |
|---|---|---|---|
| `ARTEMPLE.MAP` | 1 | 93 KB | 5123 KB |
| `ARVILLAG.MAP` | 1 | 216 KB | 5605 KB |
| `BROKEN2.map` | 3 | 767 KB | 5605 KB |

**Neither the file size nor the elevation count predicts the cost.**  Two
one-elevation maps differ by 482 KB; a three-elevation map eight times the file
of the temple costs exactly what the one-elevation village costs, to the
kilobyte.  Whatever the 482 KB is, it saturates: it is the same on a medium map
and on the largest one.

What that bounds, which nothing before did:

- the worst map measured needs **5843 KB** of the arena's 6100, started clean
- **257 KB** is all the headroom there is
- and the crash on walking into a new location came at 41 KB free, so an
  in-game transition costs about **215 KB more** than starting on the same map
  fresh.  That, and only that, is the gap to close.

**What the 482 KB is has not been identified**, and it is the next thing to
find out.  It is not objects (Broken Hills has several times the village's and
costs the same), not tiles (those are 40 KB per elevation), and not the art
cache (a fixed 1435 KB block).  A plateau reached by both a medium and the
largest map looks like a cache filling to its limit - the sound effects cache
and the script Programs are the two candidates.  `-mallocs 8 400` across the
first minute on the village would name it.

### What this changes about the work

- **`gTileIntensity` per elevation is worth much less than it looked.**  109 of
  the 155 maps have one elevation, so it saves 320 KB on most of the game - but
  the maps that need the memory are the twenty with three, where it saves
  nothing.  Narrowing it to `int16` (item 1b) saves 240 KB on every map and is
  the item to do first.
- Nothing about the art cache helps.  At 1400 KB it evicts nothing standing
  still and measures the same at 2 MB; it is 1435 KB of the 5123 baseline and
  it is not what the last 215 KB is spent on.

## An elevation change costs two megabytes, once

`KEY_F11` (patch 0013) steps the engine to the next elevation the map actually
holds, through the same three calls its own same-map transition makes.  The
first attempt measured nothing and the reason is worth keeping: it stepped
`ARTEMPLE.MAP` from elevation 0 to 1 and the temple **has no elevation 1**.
`elevationIsValid()` is only a range check against `ELEVATION_COUNT`; the header
flags say whether a level exists.  The patch checks the flags now.

On `BROKEN2.map`, which has three:

| | used | peak | with transients |
|---|---|---|---|
| standing on elevation 0 | 5605 KB | 5639 | 5843 |
| after stepping to elevation 1 | **7663 KB** | **8132** | **8293** |
| after stepping to elevation 2 | 7688 KB | 8132 | 8293 |

**The first elevation change costs 2058 KB and it does not come back.  The
second costs 25 KB.**  That is the same saturating shape as the 482 KB between
the temple and the village: something fills once and then stays, and the size
of the map has nothing to do with it.

8293 KB against an arena of 6100.  This is the whole problem, and it is more
than twice what "184 KB short" said it was.

One caveat on fidelity: F11 puts the dude on the *same tile index* on the new
elevation, where a real exit grid would name a proper tile.  If that tile is
outside the built area the renderer may be doing something a player would never
provoke.  Worth confirming once by walking in with a mouse before treating 2058
KB as the number.

### Where it goes, so far

Traced with `-mallocs 8 60` across the step, resolved to five frames: about
824 KB of it is **sound**, and nothing else was named.

| KB | frames |
|---|---|
| 228 | `_preloadBuffers` <- `soundLoad` <- `soundEffectLoad` |
| 200 | `soundLoad` <- `backgroundSoundLoad` <- `_gsound_background_play_level_music` |
| 126 | `soundLoad` <- `soundEffectLoad` <- `soundPlayFile` |
| 126 | `soundLoad` <- `soundEffectLoad` <- `soundEffectLoadWithVolume` |
| 80 | `_preloadBuffers` <- `soundLoad` <- `backgroundSoundLoad` |
| 64 | two 32 KB zlib windows, `inflate` <- `dfileReadCompressed` |

The sound effects cache is bounded - `game_sound.cc:272` passes
`cache_size << 10` and the default is 448 KB - so that part cannot run away.
The remaining ~1234 KB was below the 8 KB threshold or lost to the 60-report
budget, and a run with `-mallocs 8 400` is what closes the arithmetic.

### The art cache is not a cap, and that is a real finding

`heap.cc:912` has a `system:` label.  When the art cache's own heap cannot find
room - and it only looks in its free list and then tries to defragment, it never
evicts - it prints its stats and calls `internal_malloc` **from the arena, with
no limit**, marking the block `HEAP_BLOCK_STATE_SYSTEM`.

`heapPrintStats` has exactly one call site, that one, so every "Total free
blocks:" line in a log is the art heap failing to satisfy a request out of its
own memory.  Counted: **11 lines on the temple, 710 on Broken Hills, 206 in the
minute after an elevation step.**

Two things follow.  The art cache's size is a target, not a bound, so "1400 KB
measures the same as 2 MB" was measured on the one map where the cache never
fills and says nothing about the rest of the game.  And it means Maxim's
instinct - give the art more - is aimed at something real: memory the cache
cannot fit spills into the arena in scattered blocks that nothing accounts for.
What is not yet known is whether a bigger cache lowers the total or merely moves
it, and that is one run: `art_cache_size=3000` on Broken Hills, same step.

## Where the baseline goes, measured

The 5123 KB the temple holds, by who asked for it (`-mallocs 8 60`, resolved
against `build/apps/FALLOUT2.elf` with the loader's `XIP at 0x...` subtracted):

| bytes | asked by | |
|---|---|---|
| 1435016 | `heapInit` | the art cache, `art_cache_size=1400` |
| 307216 | `windowCreate` | 640x480, the interface window |
| 307216 | inside `gameInitWithOptions` | **unidentified**; see below |
| 243216 | `windowCreate` | 640x380, the map view |
| 64016 | `windowCreate` | 640x100, the interface bar |
| 307216 + 153616 + 142096 | `_GNW_win_refresh` | transient, at different moments |
| 168016 | `characterSelectorOpen` | transient |
| 131072 + 65552 + 65536 + 16400 | `soundLoad`, `_preloadBuffers` | |
| 32768 | `zcalloc` <- `updatewindow` <- `inflate` | one zlib window |

Two of these are worth chasing and one is settled:

- **The 300 KB inside `gameInitWithOptions` is not `_screen_buffer`.**  That was
  checked properly this time: `main.cc:274` passes `a4 = 0`, `_initWindow(1, 0)`
  passes it on, and `windowManagerInit` allocates the screen buffer only under
  `a3 & 1`.  It is never allocated.  The 300 KB is a different 640x480 buffer in
  something inlined into `gameInitWithOptions`, and without `-g` addr2line
  cannot name an inlined frame.  Link with
  `ARGON_FALLOUT_LDFLAGS=-Wl,-Map=build/fo2.map` and look up the address, or
  build that one file with `-g`.
- The three `windowCreate` buffers are 640-wide because the engine draws
  640x480.  They only shrink if the *engine* renders smaller, not if the panel
  does - see item 4.

## What else is not known about it

The 215 KB a transition needs over a clean start has not been broken down.  It
is the only thing between the game as it stands and every map in it fitting.
The instrument for it exists now: `-mallocs KB N` names the first N allocations
of KB or more in each `-memstat` window (per window, not per run, so a later
minute is not starved by startup), and `report_big` carries five return
addresses because Fallout 2's own layers eat three.  What is missing is a way to
reach a second map without a mouse - `StartingMap` starts on one but cannot
change to one.  The cheap way is another debug key beside `KEY_F11`, calling
`mapLoadByName` the way `_main_load_new` does.
