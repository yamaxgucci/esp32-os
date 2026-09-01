# Fallout 2 in 8 MB of PSRAM: what is left to do

Written 31 August 2026, branch `worktree-fallout-cxx`. Everything here is
measured; the numbers are the point, and the READMEs they came from are
`apps/fallout/README.md` under **What it would take to have room**.

## Where it stands after five patches

Shipping configuration is now `-Psram 8`, arena **6340** KB, art cache **1000**
KB, music ring 4×16 KB, patches 0014-0016, 0018 and 0019.

| map | before any of this | now |
|---|---|---|
| Temple of Trials | 5123 KB, ran | runs, with room |
| **Arroyo village** | plateau 6205, peak 6636 — **never loaded** | **runs at 22 fps, and `vsuit.mve` plays over it** |
| **Broken Hills**, the largest map in the game | **died during its own map load** | **loads and runs at 19-20 fps**, high-water 5836 of 6340; dies on the *next* map transition |

### What each change was worth

| | |
|---|---|
| 0014, bounded script cache (`-scripts N`) | 878 KB on a map that has been sat on for a while |
| 0015, stop keeping the last movie frame | 200 KB, on every map, unconditionally |
| 0016, no guard header on every allocation | ~560 KB on a busy map |
| 0018, sixteen bits of light per tile | 240 KB of `bss`, which became 240 KB of arena |
| 0019, slabs for objects and list nodes | 62 KB, and less fragmentation |
| art cache 1400 → 1000 | 404 KB |

0019 is worth reporting honestly: the estimate was 190 KB and the measurement
was 62.  The engine's own guard header was the expensive part and patch 0016 had
already taken it; what a slab removes on top of that is only whatever the ABI
allocator puts around a block, and that turns out to be about five bytes an
allocation once rounding is counted.  It is still worth having for the
fragmentation - 17 KB slabs instead of fourteen thousand small blocks - but it is
not a lever to pull again elsewhere.

## What is left

Broken Hills now dies during the map transition *out* of Broken Hills, in
`_partyMemberPrepLoad` at the head of the next `mapLoad`, on a 32 KB inflate
window with 32 KB free.  The last `[mem]` sample before it read 5836, so about
470 KB arrives between two samples: a transition holds the outgoing map while
the incoming one is being built.

Two items, in order:

1. **The world map, 739 KB, resident from startup to exit on every map.**  Still
   the largest single item in the fixed cost, and now the largest by a wide
   margin.  It is **not** the tile array, which is one block: the profiler
   counts 1585 blocks averaging 477 bytes, so it is the message list and the
   encounter tables.  Neither is needed except when the world map is on screen
   or an encounter is being rolled.  Nothing has been read yet - `wmConfigInit`
   in `worldmap.cc` is where to start, and the profiler cannot split it further
   without keying on a fifth stack frame.
2. **Narrow `Object`.**  About 170 KB across 7102 objects: `rotation`, `frame`,
   `elevation`, `lightDistance`, `lightIntensity` and `outline` do not need
   thirty-two bits.  The catch, and it is the reason this was left until last:
   `objectRead` and `objectWrite` read and write these fields directly with
   `fileReadInt32(stream, &(obj->x))`, so every one needs a temporary.  Getting
   that wrong corrupts saved maps silently.

## Things measured and closed

Do not spend runs on these again.

- **A bigger art cache.**  3000 KB instead of 1400 costs exactly its own extra
  size.  Nothing to win above ~1000 KB, and below ~800 the 505100-byte art item
  cannot be held and the engine dies.
- **Flushing the art cache**, before a map load or before a movie.  `cacheFlush`
  frees entries inside the cache's own heap, and that heap is one arena block
  allocated at startup and never shrunk, so it cannot return a byte to the
  arena.  Written twice, thrown away twice.
- **The script cap on Broken Hills.**  It never fires: fewer than twelve
  programs are resident before the arena runs out.  It pays on a map that is
  played for a while, not on one that is short of memory at the load.
- **Loading one elevation's objects.**  Worth 750 KB and blocked by the save:
  `_map_save_in_game` writes every object back, and an elevation that was never
  instantiated cannot be written.
- **`_screen_buffer`.**  Looks like 300 KB of dead allocation; never allocated,
  because `main.cc:274` passes `a4 = 0` and `windowManagerInit` needs `a3 & 1`.

## Two standing facts about the arena

**It can be raised, and only by making the image smaller.**  The shell's `mem`
says 8040 KB of extended memory is free before the app loads; the loader takes
the image's data out of that and then wants the arena as one block.  With 1770
KB of data a 6300 KB arena was refused and 6100 accepted; with patch 0018's 1536
KB of data, 6340 is accepted.  So the ceiling is roughly
`8040 - data - fragmentation`, and `bss` is the dial.  The largest objects in
`bss` after 0018 are `gObjectListHeadByTile` at 156 KB and `_square_data` at
117 KB, and both are already tight - pointers and packed tile ids.

**The framebuffer is free in QEMU and will not be on a board.**  The panel's
640x480 RGB565 buffer is 600 KB and comes from the emulated device
(`esp_lcd_rgb_qemu_get_frame_buffer` in
`components/argon_port/idf/src/panel_hw.c`).  On hardware it is PSRAM, so every
figure here is 600 KB optimistic there - which is what makes the 320x240 panel
item not optional.

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

## The bounded script cache: it works, and it is not enough

Patch 0014, `-scripts N`.  `scriptExecProc` loads a script's `Program` the first
time that script runs, under `SCRIPT_FLAG_0x01`, and nothing unloads it while
the map is up.  Everything that has to survive is in the `Script` struct, so a
program can be dropped and reloaded: clear the flag and the next call loads it
again, calls `scriptLocateProcs` to find the procedures, and runs the program's
initialisation before the procedure it was asked for.

The seam needed one addition.  `programFree` alone is not enough for a program
that has run, because `runProgram` put it on `gInterpreterProgramListHead` and
`_updatePrograms` walks that list every frame; `programListNodeFree` is static,
so there was no way to undo it.  Patch 0014 adds `programDiscard`.  The trim
itself runs from `mainLoop`, after `mapHandleTransition`, which is the one place
per frame where no script is executing, and it skips any program that is
waiting, stopped, in a critical section, already exited, or has a parent or
child.

Measured on Broken Hills, 16 MB and a 10000 KB arena so nothing fails:

| | plateau | peak |
|---|---|---|
| no cap | 7680 KB | 8131 |
| `-scripts 40` | **6802 KB** | 8131 |

**878 KB back at the plateau, and nothing off the peak** - because the peak
happens during the map load, before the count ever reaches the cap, and the
trim only runs once a frame from the main loop.

The caveat, stated rather than hidden: a Fallout script's file-scope variables
live in the `Program`, so evicting one resets them.  Persistent state is meant
to use `local_var`/`global_var` and every program is dropped on a map change in
any case, so nothing can rely on them across visits - but something may rely on
them within one.  That is why the default is 0, meaning no cap.

## And Broken Hills still does not load on 8 MB

The test that matters: arena 6100, `-Psram 8`, `-scripts 24`, and the art cache
turned down.  Both attempts die the same way, during the map load, before the
map's own script has printed anything:

| art cache | free when it failed | what asked |
|---|---|---|
| 800 KB | 30 KB | 32768 bytes |
| 600 KB | **8 KB** | 32768 bytes |

Two things follow, and the second is the useful one.

**The failing block is a zlib inflate window.**  32768 bytes, `zcalloc` <-
`updatewindow` <- `inflate` <- `dfileReadCompressed`, one per compressed `.DAT`
entry being read.  It is the straw, not the load: any 60 KB found elsewhere
would let it through.

**Turning the art cache down makes things worse.**  200 KB smaller left 22 KB
*less* free at the moment of failure.  That is the `system:` fallback in
`heap.cc:912` earning its keep: what will not fit in the cache goes to the arena
in scattered blocks, so shrinking the cache does not shrink the demand, it just
moves it somewhere with more overhead.  Together with the earlier run at 3000
KB, which cost exactly its own extra size, **the art cache is not a lever in
either direction.**  Leave it at 1400 and stop measuring it.

## How to instrument this

`-memstat N` prints the arena every N ms and arms the rest.

`-mallocs KB N` names the first N allocations of KB or more **per window**
rather than per run, with five return addresses - the engine reaches malloc
through `memoryBlockMallocImpl`, `internal_malloc`, the allocator registered
with its memory manager and sometimes `internal_malloc_safe`, so three frames
name nothing but plumbing.

`-prof N` counts what is **still held** per call site and prints the N biggest.
This is the one that answered the question: counting what was asked for measures
churn and nothing else, because the engine asks for 37 MB in a three-second
window and gives nearly all of it back.  Its tables are 768 KB of bss, which an
8 MB part cannot spare on top of a 6100 KB arena - the app does not load at all
- so it is off unless the build asks:

```bash
ARGON_LIBC_MALLOC_PROFILE=1 argon fallout --game 2 --rodata-in-code
```

To reproduce an 8 MB failure *with* the profiler, keep `ARENA_KB` at 6100 and
run `-Psram 16`: the arena pressure is the same and the extra bss has somewhere
to live.

`-scripts N` caps resident script programs.  `-fps N` prints where a frame goes.
`KEY_F11` (patch 0013) steps to the next elevation the map actually has.

## The complete accounting

Broken Hills, arena 6100, `-scripts 24`, at the moment the load fails.  Built
with `ARGON_FALLOUT_DEBUG=1` so `addr2line -i` names the inlined callee instead
of `gameInitWithOptions`, which is where this engine puts every init routine:

| KB live | blocks | site |
|---|---|---|
| 1192 | 14204 | `objectLoadAll` — `map.cc:922` |
| 798 | 3726 | `wmWorldMap_init` — `game.cc:280` |
| 602 | 5 | the art cache heap — `art.cc:151` |
| 421 | 23 | `isoInit` — `game.cc:235` |
| 363 | 14046 | `protoInit` — `game.cc:249` |
| 310 | 1 | `directDrawInit` — the engine's 640×480 8-bit surface |
| 300 | 5 | `falloutMain`, including `_main_load_new`'s loading window |
| 209 | 7054 | `aiMessageListReloadIfNeeded` — `combat_ai.cc:3610` |
| 146 | 5 | `cacheLock` — art that spilled out of the cache into the arena |
| 108 | 5 | `interfaceFontsInit` |
| 106 | 6 | `protoInit`, a second site |
| 85 + 20 | 13 | `isoInit` |
| 79 | 924 | `gameInitWithOptions` itself — `main.cc:274` |
| 72 | 32 | `mapLoadByName` |
| 64 | 1 | `mapLoad` — `map.cc:833` |
| 55 | 1 | `aiMessageListInit` |
| 51 | 2606 | `protoInit`, a third site |

5397 KB of the ~5880 in use, in 47062 tracked blocks.

**Read the block counts, not just the sizes.**  Three sites account for 35304 of
those blocks: the objects, the prototypes and the AI messages.  Fallout 2's
`internal_malloc` puts a 16-byte header on every one, and the ABI allocator
underneath adds its own and rounds up — so on the order of **560 KB of that
5397 is per-block overhead on tens of thousands of tiny allocations**, not data.
The prototypes are the clearest case: 363 KB in 14046 blocks is 26 bytes each,
which is mostly header.

## The art cache has a floor, and it is not 600 KB

The 8 MB run dies like this:

```
Heap Warning: Could not allocate block of 505100 bytes.
Total free size: 505872        <- in four fragments
Allocating block from system memory...
malloc: out of arena asking for 505136 bytes; 222 of 6100 KB free
fatal error: internal_malloc() failed in heap_find_free_block()!
```

**One art item is 505100 bytes.**  A 600 KB cache cannot hold it once the cache
has any holes in it, so the request goes to the `system:` fallback, the arena
has 222 KB, and the engine calls that fatal.  That is the mechanism behind the
earlier oddity where turning the cache *down* left less free at the moment of
failure, not more.

So `art_cache_size` has a hard floor: comfortably more than the largest single
`.FRM` the game will ask for, which is at least 505 KB.  1400 KB was a good
choice.  **Do not lower it** - and there is nothing to win by raising it either
(measured: 3000 KB costs exactly its own extra size).  This question is closed.

## The levers, largest first

1. **The world map: 798 KB, resident from startup to exit, on every map.**
   `wmWorldMap_init` at `game.cc:280`, 3726 blocks.  The player looks at the
   world map for a few seconds at a time; nothing needs its cities, its tile
   descriptions and its encounter tables resident while walking around Broken
   Hills.  Loading it when `wmWorldMap()` is entered and freeing it on the way
   out is the single biggest item on this list, bigger than anything else
   available.
2. **The per-block overhead: on the order of 560 KB.**  A pool for the three
   sites that make 35304 tiny allocations - `Object` and `ObjectListNode` in
   `objectLoadAll`, the prototype lists in `protoInit`, the AI message lists -
   removes the 16-byte engine header and the allocator's own from each.  All
   three are long-lived and freed wholesale, which is exactly the shape a bump
   allocator suits.
3. **Scripts: 878 KB, already implemented.**  Patch 0014, `-scripts N`.  What is
   missing is enforcing the cap *during* the map load rather than once a frame
   from `mainLoop` - the peak happens before the count ever reaches the cap.
4. **The last movie frame: 200 KB, done.**  Patch 0015.
5. **Objects, narrower.**  On top of pooling them: `rotation`, `frame`,
   `elevation`, `lightDistance`, `lightIntensity` and `outline` do not need to
   be `int`.  About 170 KB across 7102 objects.
6. **`isoInit`, 421 KB in 23 blocks.**  Not looked at yet; 23 blocks of ~18 KB
   is worth one read of `map.cc:227` onwards.

Together 1 and 2 are about 1350 KB, which is the size of the hole.

## What reading the source settled that measuring could not

Worth recording, because the balance went both ways today.

Measuring was the only way to learn **how big** anything was, and it overturned
three conclusions that reading had supported: `_screen_buffer` looked like 300 KB
of dead allocation and is never allocated; an elevation change looked free in
the code and appeared to cost 2 MB, which turned out to be the map filling up
over minutes; and map size looked like it must dominate, while the largest map
in the game costs the same as a medium one at the moment it finishes loading.

Reading was the only way to learn **what** anything was, once a number pointed
at it.  `_lastMovieBuffer` is the clean case: the profiler could say "200 KB
from somewhere near `_cleanupMovie`" and no more, and five minutes in `movie.cc`
showed it is written once and read nowhere, which made patch 0015 a two-line
certainty instead of a guess.  Same for the 310 KB, which is
`SDL_CreateRGBSurface(0, 640, 480, 8)` in `directDrawInit` and cannot go
anywhere while the engine draws at 640×480.

The tool that made the difference in between was `-g`.  Without debug info
addr2line answers with whatever symbol owns the enclosing section, which for an
inlined callee is the wrong function - it named one address `gameInitWithOptions`
on one run and `strParseStrFromFunc` on the next.  The link map is no better: it
lists no static functions, so its nearest-preceding answer for
`memoryBlockMallocImpl` was `mapLoadSaved+0x255`.  With `ARGON_FALLOUT_DEBUG=1`
every site above came out with a file and a line on the first try.


## Since the first five patches: what changed and what was ruled out

**0021 `-artcache N`**, and the reason it exists: the right cache size cannot be
found by a run with `-autostart`, and it turned out it could not be found by any
of mine. Every earlier measurement was made on the Temple of Trials, the
smallest map in the game, where a cache over a megabyte reads nothing at all.
On Arroyo village, `-autostart`, nobody at the keyboard:

| cache | reads per 30 s window | evictions |
|---|---|---|
| 1400 KB | 615-732 | 630-729 |
| 1800 KB | 54-195 | 48-178 |

Maxim's own play session at 1400 had **10153 evictions and 18 full flushes** in
it, and felt like five frames a second while the counter said twenty. The
default is 1800 now, funded by 0020.

**0022 releases the movie decoder's buffers.** MVE keeps three - the file record
buffer and two frames - and for a 640x320 movie the two frames are 409600 bytes.
A live-block census on Broken Hills, with the opening movies long finished,
found 419 KB in three blocks still held by `movieMallocImpl`. `MVE_ReleaseMem`
returns them through `_cleanupMovie`, by a path that depends on how the movie
ended; 0022 asks for it in `gameMoviePlay` as well, where the game is done with
the movie for certain. Idempotent, and verified: `movieMallocImpl` is gone from
the census.

**0023 counts what the art heap takes out of the arena, and the answer is
nothing.** Three attempts to give BROKEN2.MAP more room - a smaller cache, 250
KB from the world map, 419 KB of movie decoder - all ended at the same wall with
the same twenty-odd kilobytes free, which looks exactly like something taking
whatever it is given. The candidate was `heapFindFreeBlock`'s `system:` label.
Measured on that map: **spilled 0 KB in 0 blocks**, right up to the failure. The
hypothesis is dead and no behaviour was changed to chase it.

The same patch stops the label printing 500 bytes of heap statistics on every
failed probe rather than only when a block is taken - 710 of those on Broken
Hills against 11 on the temple.

### The tools, and they work on a board

| flag | what it is for |
|---|---|
| `-hud` | frames, arena used and largest free block, drawn into the panel in a 3x5 font. The last is what decides whether a dialogue opens: its backdrop is 300 KB in one piece |
| `-log PATH` | every diagnostic line and every engine error to a file. The shell's `>` cannot do it: `run` hands the process to the session and returns, and the redirection goes with it - measured. **On a board point it at `c:\` or `a:\`**: `h:\` is QEMU's host folder and exists only with `-HostFs`, and pointing at it elsewhere produces no log and no complaint |
| `-artcache N` | the cache size, without rebuilding the card |
| `-smooth` | average each 2x2 when the panel is smaller than the picture |
| `-scripts N`, `-fps N`, `-memstat N`, `-mallocs KB N`, `-prof N` | as before |
| `--psram 16` | build for the larger part: arena 12000 KB, nothing else changes |

### 320x240

Works. `C:\SYSTEM.CFG` with `width = 320`, `height = 240`, and the port says
`SDL: panel 320x240, picture 640x480, reducing by 2`. The engine still draws
640x480; the blit reduces. In QEMU the memory figure does not move because the
framebuffer belongs to the emulated device, but **on hardware the panel is 150
KB instead of 600.**

`-smooth` is what makes the text readable. Picking the pixel of each 2x2 that
differs most from the block average was tried first, on the reasoning that a
light stroke on dark and a dark stroke on light would both survive - and
measured worse against nearest-neighbour on the interface bar, because on a
textured panel the outlier is a speck of background more often than a stroke.
Averaging cannot lose a stroke, only dim one. 27.1 fps against 27.9.

### Still open

- **BROKEN2.MAP** runs out during play, and now for a plain reason: 7102 objects
  at about 148 bytes. Narrowing `Object` is ~170 KB and touches `objectRead`
  and `objectWrite`, which read those fields as int32 straight from the map
  file.
- **Movies play through the wrong palette** (grey). Narrowed: the callback
  fires with real colour, the port builds a colour table from it, and the map
  goes through the same path correctly. Next step is to watch it in a window
  rather than bisect it headless.

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

Each map started directly with sfall's `StartingMap` in `ddraw.ini` and
`-autostart`, so it is weighed with nothing else ever resident.  16 MB of PSRAM
and a 10000 KB arena, so nothing fails and the numbers are the demand rather
than what happened to fit.

**Read the whole run, not the first minute.**  Every map reports about 5605 KB
the moment it finishes loading and then keeps growing for several minutes
before it settles.  Two conclusions were drawn and then withdrawn because of
this — see "Two wrong answers" below — so the number that matters is the
plateau:

| map | elevations | file | just after loading | plateau | peak |
|---|---|---|---|---|---|
| `ARTEMPLE.MAP` | 1 | 93 KB | 5123 KB | **5123, flat** | 5493 |
| `ARVILLAG.MAP` | 1 | 216 KB | 5605 KB | **~6205** | 6636 |
| `BROKEN2.map` | 3 | 767 KB, the largest in the game | 5605 KB | **~7680** | 8131 |

Against an arena of 6100 KB:

- the Temple of Trials fits, with 600 KB to spare, and never grows
- **Arroyo village already does not**: 6636 KB at its peak
- **Broken Hills needs 2 MB more than exists**

So it is not that the game is a little short on one transition.  Everywhere
past the starting map is over the line, and the largest maps by 2 MB.

The growth is gradual, needs no input at all — `big.log` and `live.log` reach
the same 8131 KB with nothing touched — and it **plateaus**, which is the shape
of a working set filling rather than a leak.

Every map's header says which of the three elevations it holds; a *set* bit in
`flags` means the level is **absent** (`_map_data_elev_flags` in `map.cc`, and
the tile reads at 1417 and 1727 which run when the bit is clear).  Decoded for
all 155 maps in `master.dat` and `critter.dat`: **109 have one elevation, 26
have two, 20 have three.**

## Two wrong answers, and what was wrong with them

Both came from reading a run too early, and both are kept here so the next
person does not repeat them.

**"The size of a map does not matter."**  Broken Hills and Arroyo village both
report 5605 KB after loading, and Broken Hills is eight times the file.  They
diverge over the following minutes to 7680 and 6205.  A map's cost is not what
it takes to load.

**"An elevation change costs 2058 KB."**  `KEY_F11` (patch 0013) steps the
engine to the next elevation the map actually holds, and on Broken Hills the
arena went from 5605 KB to 7663 within a minute of pressing it — twice, on two
separate runs.  It was the climb, not the keypress: `big.log`, which never
received an F11, reaches 8131 KB on its own.  The step may still cost
something; nothing here measures it, because the thing being measured moves by
2 MB while standing still.

A third, smaller correction: the first F11 attempt reported no change because
it stepped `ARTEMPLE.MAP` from elevation 0 to 1 and the temple **has no
elevation 1**.  `elevationIsValid()` is only a range check against
`ELEVATION_COUNT`.  The patch reads the header flags now.

## The art cache is not a cap, and a bigger one does not help

`heap.cc:912` has a `system:` label.  When the art cache's own heap cannot find
room — and it only looks in its free list and then tries to defragment, it
never evicts — it prints its stats and calls `internal_malloc` **from the
arena, with no limit**, marking the block `HEAP_BLOCK_STATE_SYSTEM`.
`heapPrintStats` has exactly one call site, that one, so every "Total free
blocks:" line in a log is that fallback: **11 on the temple, 710 on Broken
Hills.**

That is a real mechanism for unbounded arena growth and it is the best
candidate for the climb.  What it is not is something a bigger cache fixes —
`art_cache_size=3000` instead of 1400 on Broken Hills:

| art cache | just after loading | plateau | heap fallbacks |
|---|---|---|---|
| 1400 KB | 5605 KB | ~7680, peak 8131 | 710 |
| 3000 KB | 7210 KB | ~9280, peak 9739 | 23 |

The two runs differ by 1605 KB after loading and 1608 at the plateau, which is
the 1600 KB of extra cache and nothing else.  **A bigger cache costs its own
size and changes the total by nothing** — even though it nearly stops the
fallback firing.  Which is itself informative: whatever the climb is, it is not
blocks a larger cache would have kept inside itself.

So the art cache should be as **small** as the frame rate tolerates, not as
large as the arena allows.  1400 KB down to 800 is 600 KB back, and what that
costs in frames has only ever been measured on the temple, where the cache
cannot fill.

## What the climb is: objects and scripts

Answered, and confirmed by two runs that agree on everything else.  The
live-bytes profiler (`-prof N`) counts what is still outstanding per call site
rather than what was asked for — the engine asks for 37 MB in a three-second
window and gives nearly all of it back, so counting requests measures churn and
nothing else.  At the plateau, with `PROFILE_MIN` at 96 bytes:

| site | Temple of Trials | Broken Hills | difference |
|---|---|---|---|
| `artInit` — the art cache heap | 1401 KB | 1401 KB | 0 |
| three sites inside `gameInitWithOptions` | 1172 KB, 487 blocks | 1172 KB, 487 | 0 |
| one 640×480 buffer (see below) | 310 KB, 1 | 310 KB, 1 | 0 |
| a block near `_cleanupMovie` | 200 KB, 1 | 200 KB, 1 | 0 |
| `interfaceFontsInit` | 108 KB | 108 KB | 0 |
| `protoInit` | 106 KB | 106 KB | 0 |
| `isoInit` | 85 KB | 85 KB | 0 |
| sound | 201 KB | 269 KB | +68 |
| **`mapLoad` — the map's objects** | **81 KB, 567 blocks** | **1026 KB, 7102 blocks** | **+945** |
| **`programCreateByPath` — scripts** | **not in the top 16** | **1025 KB, 160 blocks** | **+1025** |
| total attributed | 4047 of 5330 | 6155 of 7681 | |

**Two things scale with the map and nothing else does.**  1970 KB of the 2108 KB
difference is objects and scripts.  Everything else is a fixed cost paid on
every map, the Temple of Trials included.

`mapLoad`'s blocks average 148 bytes, which is `sizeof(Object)` plus the
engine's own 16-byte block header: 7102 objects on Broken Hills against 567 in
the temple.  `objectLoadAll` (`object.cc:502`) reads the objects of **all three
elevations** in one pass, so a three-elevation map pays for all of them at once.

Scripts are the other half, and they are why the arena climbs for minutes after
a map loads rather than arriving with it.  `scriptExecProc`
(`scripts.cc:1275`) loads a script's `Program` **the first time that script
runs**, guarded by `SCRIPT_FLAG_0x01`, and nothing unloads it while the map is
up.  So the resident set grows as critters wake, doors get looked at and timers
fire, and it settles once every script on the map has run once: 160 programs,
6.4 KB each.

## The plan, with the numbers behind it

The gap: Broken Hills plateaus at 7680 KB and peaks at 8131 against an arena of
6100.  About 1800 KB has to come out, and these four are where it is.

### 1. A bounded script cache — up to 800 KB

The biggest single item and the one with a clean seam.  A script's persistent
state lives in its `Script` struct, not in its `Program`: `procs[]`, flags,
owner, target, the lot.  `programFree` already exists and frees `data`,
`dynamicStrings` and the stacks, and the reload path is complete — clear
`SCRIPT_FLAG_0x01` and the next `scriptExecProc` reloads the program, calls
`scriptLocateProcs` to re-find the procedures, and runs the program's
initialisation before the procedure it was asked for.

So: walk `gScriptLists` the way `scriptGetSid` does, count resident programs,
and when the count is over a cap, free the programs that are not currently
executing.  160 programs down to 40 is about 770 KB.

The risk, stated rather than hidden: a Fallout script's file-scope variables
live in the `Program`, so evicting one resets them.  Persistent state is meant
to use `local_var`/`global_var`, and scripts are unloaded wholesale on a map
change anyway, so nothing can rely on them across visits — but something might
rely on them within one.  Worth a config switch rather than being
unconditional.

### 2. The art cache down, not up — 600 KB

`art_cache_size=1400` is 1401 KB of the fixed cost, and a bigger one buys
nothing (measured: see above).  800 KB gives 600 KB back.  What it costs in
frames has only ever been measured on the temple, where the cache cannot fill,
so this needs one `-fps` run on Broken Hills before and after.

### 3. The 310 KB buffer, and the 200 KB one — up to 510 KB

Both are single blocks, both are live on every map from startup to exit, and
neither is identified.  The 310 KB one is 307216 bytes, which is 640×480 plus
the engine's header — a whole screen.  It is **not** `_screen_buffer`: that is
guarded by `a3 & 1` in `windowManagerInit` and `main.cc:274` passes `a4 = 0`.
addr2line named it `gameInitWithOptions` in one run and `strParseStrFromFunc`
in another, which means the symbol belongs to a cold or cloned section and
cannot be trusted: the app is built without `-g`.  Link with
`ARGON_FALLOUT_LDFLAGS=-Wl,-Map=build/fo2.map` and look the address up there,
or build that one translation unit with `-g`.

The 200 KB one resolves to a `constprop` clone near `_cleanupMovie` and is
still live long after the movies have finished, which is either a movie buffer
that is never freed or another bad symbol.  Same treatment.

### 4. Objects, without changing when they load — about 280 KB

Loading only the current elevation's objects would be worth 600 KB on a
three-elevation map, and it runs into the save: `_map_save_in_game` writes
every object back, so an elevation that was never instantiated cannot be
written and the map would lose it.  Not impossible, but not cheap either.

Cheaper and safe, for 7102 objects:

- they are allocated one at a time, so **113 KB is the engine's 16-byte block
  header**.  A slab or pool for `Object` recovers all of it.
- the struct is nineteen `int`s and several are small: `rotation`, `frame`,
  `elevation`, `lightDistance`, `lightIntensity`, `outline`.  Narrowing the
  obvious ones is roughly another 170 KB.

### Not on this list any more

- **`gTileIntensity` and `_square_data`** are `bss`.  `bss` is in the app's
  data segment, which the loader allocates separately from the arena, so
  narrowing them does not give the arena a single byte.  They make the *image*
  smaller, which matters for a different reason and on a different page.
- **A bigger art cache.**  Measured; it costs its own size exactly.
- **Anything about the map ambience or the effects cache.**  Already streamed,
  already 64 KB.

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

## A visual check, and one thing it turned up

Patches 0016, 0018 and 0019 touch the engine's allocator, its lighting array and
its object allocation, which is the sort of change that can leave a game running
at the right frame rate and drawing the wrong thing.  So: `-shot h:\map.ppm
-shotms 470000` with `-HostFs` pointing at a host folder writes the frame
straight out of the guest.

Arroyo village came out correct - full colour, correct lighting, the interface
bar, the critters, the messages about passing the Temple of Trials.  82% of
sampled pixels carry a colour cast, which is what a palette-mapped scene should
look like.

**A movie frame did not.**  A shot at 330 s caught `intro.mve` and came out
grey: 0.0% of sampled pixels have a channel spread over 6, and the largest
spread in the whole frame is 4, which is RGB565 rounding.  That is the identity
ramp `directDrawInit` installs when it creates the surface, `colors[i] = {i, i,
i}`, and not a desaturated movie.

What was ruled out, so the next person starts further along:

- The screenshot is not lying about the screen.  `write_ppm` reads `s_gfx.fb`,
  the panel framebuffer itself.
- The engine's callback fires, and with real colour.  `movieSetPaletteEntriesImpl`
  is called with `start=1 count=254`, and the entries carry warm browns -
  `e60=20,15,9`, `e120=40,39,28` in the palette's own six-bit units.
- The port receives colour and builds a colour table.  Sampling
  `palette_lut` at rebuild gives `c60=184,156,40`, `c200=0,108,0` and others.
- Nothing to do with this session's patches: the map screenshot is 82% coloured
  through the same path.

What is left, and it is an ordering hazard specific to the movie path.  In
`movie_lib.cc`'s frame opcode the sequence is `palClrPalette` (black), then
`sfShowFrame`, then `palSetPalette` (the real colours) - and `movieDirectImpl`
calls `renderPresent()` itself, inside `sfShowFrame`.  In this port the palette
is applied during the 8-bit-to-RGB565 blit and the presenting surface *is* the
framebuffer, so a palette set after the present cannot change the frame already
on the panel.  The game's normal frames set the palette and then draw, which is
why they are fine.

That predicts black rather than grey, so it is not the whole story.  The cheapest
next step is not another headless bisection: run it in a window - `argon run -Sd`
- and watch what the movies actually do.

## What each change costs in function, not just in bytes

Asked directly, and worth having in one place.

| patch | saved | what it takes away |
|---|---|---|
| 0014 bounded script cache | 878 KB on a map that has been sat on | **Something, possibly.** An evicted `Program` loses its file-scope variables. Fallout keeps persistent state in `local_var`/`global_var` and drops every program on a map change anyway, so nothing can rely on them across visits — but something may within one. Off by default (`-scripts 0`); it is a flag, not a policy |
| 0015 last movie frame | 200 KB, every map | Nothing. `_lastMovieBuffer` is written and read nowhere — the only other mention in the engine is `movieExit` freeing it |
| 0016 no guard header | ~560 KB on a busy map | **A debug check.** `memoryBlockValidate` can no longer catch a write past the end of a block, and `mem_check()` prints the block count and says the byte total is not tracked. Nothing the game does depends on either |
| 0018 sixteen-bit light | 240 KB of bss, which became arena | **Almost nothing.** The light quantum is 4 of 65536, a two-hundred-and-fiftieth of a palette shade. It saturates only with three full-intensity lights on one hex |
| 0019 object slabs | 62 KB, and less fragmentation | Nothing. A different allocator for the same objects |
| 320×240 panel | 450 KB **on hardware**; nothing in QEMU | **A great deal, on purpose.** The interface text is laid out for 640×480 and halving it makes it hard to read. This is about running on a CYD at all |

And one that was reverted: art cache 1400 → 1000 saved 404 KB and cost the frame
rate and three dialogs. See the note in `fallout2.cfg`.

## 320×240 works

Measured, not just compiled. `C:\SYSTEM.CFG` with `width = 320`, `height = 240`,
and the port says what it is doing:

```
SDL: display 320x240
SDL: panel 320x240, picture 640x480, reducing by 2
```

The screenshot is the Temple of Trials at 320×240, in colour, 68.6% of sampled
pixels carrying a colour cast, with the interface bar whole at the bottom.  The
game runs; the memory figure is unchanged at 5420 KB of 6340 because in QEMU the
framebuffer belongs to the emulated device either way.  **On hardware the panel
is 150 KB instead of 600**, and that 450 KB is the arena's.
