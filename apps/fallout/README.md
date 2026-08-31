# Fallout (`FALLOUT.AXE`, `FALLOUT2.AXE`)

[fallout1-ce](https://github.com/alexbatalov/fallout1-ce) and
[fallout2-ce](https://github.com/alexbatalov/fallout2-ce) — Alexander Batalov's
reverse-engineered Fallout 1 and 2 — as ArgonOS `.AXE` images. Compiled for the
ESP32-S3, running from flash XIP with the data in PSRAM.

**Fallout 2 runs on an ESP32-S3.** It opens the 326 MB `MASTER.DAT` off a FAT32
card, reads its 23120-entry index out of flash, loads the art and protos and
scripts, opens the audio at 22 kHz, comes up at its main menu with its music,
takes a keypress, makes a character and stands on the map of the Temple of
Trials. The opening movies play at 640×480 with no frames dropped when they are
not skipped - 2502 of `intro.mve`.

**In 8 MB of PSRAM**, with 606 KB to spare, measured rather than argued: see
**Where the memory goes**.  Standing on the map it holds **27 frames a second**
in QEMU, median of 210 three-second windows; it was 6.2 before two things that
had nothing to do with drawing - see **What it spends its time on**.

Input works: the port opens `/dev/kbd0` and `/dev/mouse0` and reads them every
frame, `tools/inputplay.py` presses keys with nobody at the desk, and
`run a:\sdltest.axe -input 45` is the ninety-second check that all of it still
arrives.

| | Fallout 1 | Fallout 2 |
|---|---|---|
| translation units compiled | 108 of 108 | 126 of 126 |
| code, after `--gc-sections` | 453 KB | 745 KB |
| data (mostly bss) | 1.82 MB | 1.87 MB |
| extended memory asked for | 5.75 MB | 6100 KB |
| game data on the card | — | 562 MB |
| `.DAT` index, in flash rather than memory | — | 1331 KB |

**Where it is.** With the archives on the card the engine runs the whole of
`game_init`, opens its window, loads its music and reaches the main menu:

```
fallout2-ce on ArgonOS, argv[0] = /sd/fallout2.axe
sfall: ddraw.ini read
fallout: master.IDX mapped, 23120 entries, 1012 KB in flash
fallout: critter.IDX mapped, 7120 entries, 319 KB in flash
SDL: display 640x480 with a framebuffer, 1280 bytes per row
SDL: audio 22050 Hz, 2 channels, blocks of 1024 frames
>gsound_init  >initMovie  >gmovie_init  >moviefx_init  >art_init
>tile_init  >obj_init  >cycle_init  >proto_init  >scr_init
Loading background sound file 07desert.acm... playing
[mem] 5082 KB used of 6100, peak 5082 (5311 with transients)
```

Without the archives it stops at `cache_init failed in art_init`, which is
`iso_init` finding no art; everything the shim can be shown to do without a
copy of the game, `SDLTEST.AXE` shows.

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

```
argon fallout --game 2
```

`argon` puts the IDF toolchain on PATH first, which `build.py` needs and which
is three lines of PowerShell to get right by hand - and getting them wrong
fails as `FileNotFoundError` out of `subprocess`, with no mention of a
compiler.  `python apps/fallout/build.py` still works in a shell that already
has `xtensa-esp32s3-elf-gcc` on PATH.

First run clones the engine and its two dependencies (about 40 MB), then
compiles 118 objects and links `FALLOUT.AXE` into `build/apps/`.

### Our changes to the engine

`third_party/` is a clone and is in `.gitignore`, so an edit made there is
invisible to this repository and gone the moment anyone deletes it and fetches
again.  The changes live in `apps/fallout/patches/<engine>/` instead, applied
on every build and skipped when the tree already has them.  A patch that no
longer applies stops the build and names the file - which is the point: the
engine is somebody else's, it moves, and a change of ours that has quietly
stopped taking effect is worse than one that fails loudly.

```
argon fallout --game 2 --rodata-in-code
```

Links the image's 97 KB of constants with the code, which is executed from
flash, so they cost no PSRAM.  Off by default and worth thinking about before
turning on: they then arrive over the same bus and through the same cache as
the instructions, and 745 KB of code is already working that cache hard.  It
buys memory with frame rate, and on this port the frame rate is the thing that
was already tight.

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

Measured, not estimated - `--survey` for the first four, the linker for the
rest.  Fallout 1, which is the smaller of the two:

| | |
|---|---|
| translation units that compile unmodified | **108 of 108** |
| code before the linker looks | 560 KB |
| code in the image, after `--gc-sections` | **439 KB** |
| data + bss | **1.73 MB** (287 KB stored, 1.45 MB zeroed) |
| relocations | 5109 |
| stack actually used, through `game_init` | 4.8 KB of 16 KB |
| frame rate of the engine's own palette fade, in QEMU | ~57 fps |

Neither image's code fits the 192 KB arena, so the loader puts it in flash and
executes it in place.  That is automatic - the image asks for nothing.  The
data goes to PSRAM, and how much of it there has to be is
**Where the memory goes** below.

The engine source was **unmodified** for the whole of the port: not one patch,
not one `#ifdef ARGON`.  Everything that had to give way gave way on this side -
`port/SDL.h` is the SDL2 subset the engine actually uses, `port/dirent.h`
shadows the toolchain's so that `d_type` exists, and the rest is in `port/`.

There are five patches now, all in `apps/fallout/patches/fallout2-ce/`, and not
one of them is a portability fix.  Three are about a machine with six megabytes
of arena - the `.DAT` index read from flash, a music buffer that held nine
seconds in advance, and a map's ambience decoded whole into memory.  One is
about a card rather than a disk: the archive was reopened for every file inside
it.  The fifth makes the engine say whether it found `ddraw.ini`.  Everything
that *could* be done from outside still is - the largest speed-up of all is a
line in `fallout2.cfg`.

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

**`C:`, not the card.** The file is staged on the card for convenience and the
display driver never looks there:

```
copy a:\system.cfg c:\system.cfg
```

then reboot. Naming the destination file rather than the directory is not
style: a command given to `argon test` must not end in a backslash, because
PowerShell escapes it into the next argument and the guest is handed the two
glued together - which copies *something*, reports success, and leaves the
panel the size it was.

Getting the size wrong costs 600 KB rather than looking wrong: with a panel of
another size the presenting surface cannot *be* the framebuffer, so the shim
allocates its own 640×480 one out of the arena and copies into it every frame.
Measured, this run against the last: 5594 KB at the main menu instead of 6202.
The shim says which of the two happened at startup - "with a framebuffer"
against a line naming both sizes - and `SDLTEST.AXE` prints `presenting surface
is the panel`.

**And a rebuild takes it away.** `C:` is littlefs in the firmware image, so
building the firmware replaces it and the file is gone - and `argon test`
builds the firmware before every run.  Change one line of the kernel and the
next run is 608 KB poorer than the one before it, with nothing in the output
saying so: the game prints `SDL: display 640x400` once among fifty lines of
boot, and dies four screens later asking for a block of memory it would
otherwise have had.

So a measuring run after any source change is two runs.  The first puts the
file back and costs half a minute:

```
argon test -Sd -SdImage build\fo2card.img -TimeoutSec 150 "copy a:\system.cfg c:\system.cfg" "type c:\system.cfg"
```

The second changes nothing, so nothing is rebuilt, so `C:` survives into it.
`[mem]` right after `art_init` is the check that it worked: **343 KB with the
panel, 951 KB without**.

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
argon fallout --game 2
```

The archives' indices are computed once, on the host, and go on the card beside
them. Skip this and the game still runs; it just spends 1.7 MB of the arena and
ten seconds rebuilding them at every start.

```
python apps/fallout/tools/mkdatidx.py build\fo2_card\master.dat build\fo2_card\critter.dat
```

```
argon sync build\fo2_card -SizeMB 1024 -Image build\fo2card.img
```

```
argon test -Sd -SdImage build\fo2card.img -Psram 8 "run a:\fallout2.axe"
```

### Playing it without hands

`drv install a:\kbdvirt.sys` and `a:\mousevirt.sys` first, then
`tools/inputplay.py` presses the keys. Wait for the guest to reach the screen
you mean to type at rather than counting seconds at it: `qemu-boot` writes its
transcript as the run goes, so the log itself is the signal. The main menu
announces itself by loading its music.

```bash
base=$(wc -c < build/fo2.log)
until tail -c +$base build/fo2.log | grep -aq 07desert; do sleep 5; done
python tools/inputplay.py --wait 120 "key n" "wait 25000" "key t"
```

From the current end of the file, and with `-LogPath` giving the run a log of
its own.  Grepping the whole of a shared transcript finds the *previous* run's
menu and fires at once, which puts every key into the loading screen.

`n` is New Game and `t` takes a premade character; both are the engine's own
shortcuts. **Not `esc` at the main menu** - there it means Exit, and a script
that opens with a few hopeful escapes quits the game before it starts.

Timing it instead of watching for it fails in a way worth knowing about: keys
sent before the game opens `/dev/kbd0` are not lost any more, they are
*delivered in a burst the moment it does* - which is during the loading screen,
where they do nothing at all.

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

**`-Psram 16`.** For measuring, not for playing.  A ceiling the game can reach
is a ceiling that answers instead of the question: with 16 MB the high-water
mark is the game's own, and `--arena-kb 12000` with it.  `-Psram 8` is the real
target and what **Where the memory goes** is about.

**`--arena-kb`.** The process asks for its extended memory outright rather than
taking the default quarter-of-what-is-free, and the number is what the
measurement says it needs.  See the comment on `AG_APP_SIZED` in
`fallout_main.cc`.

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

## What it spends its time on

`-fps N` prints where a frame goes, every N milliseconds:

```
[fps t=142414] 3.03 fps, 329483 us/frame: blit 78 (16 KB), flush 1 in 66 (8 KB),
               engine 329339, io 211678 us (open 4102, seek 1210, read 206366)
               (523 reads, 0 opens of which 0 found nothing, 228 seeks, in the
               window)
```

The blit is the game's picture converted through its palette, the flush is
handing it to the display, and what is left is the engine - of which the
filesystem's share is counted separately, because a frame waiting on a card is
a different problem from a frame thinking.  Counts are totals over the window;
times are per frame.  The guest's clock is in the line so the engine's own
progress printing can be timed against it.

The filesystem's share is split three ways, because those three have nothing in
common to fix.  Time in `open` is the card being searched for a name and is
what the VFS's negative cache is for; time in `seek` is the FAT being walked
and is what fast seek was for; time in `read` is bytes, and the only lever on
bytes is asking for fewer.  With one number there was no way to ask whether the
lookups still cost anything after they were cached, which is what the split was
added to answer.

A second line follows it with what the engine's own caches are doing:

```
[art t=451567] 0 read, 0 evicted, 0 flushed, largest 307303;
               hexdist 42 calls, 1008 steps, 0 gave up
```

Read, evicted and flushed are the art cache, and they are three different bugs
with three different fixes - a cache that is too small, an eviction policy that
throws out more than it needs, and a whole cache emptied because one entry
would not fit.  `hexdist` is the tile-distance walk: calls, tiles stepped, and
walks that hit their bound.

The first `-fps` line ever printed ended the search for a drawing problem: 3 ms
of graphics in an 845 ms frame.  Nothing that follows would have been found by
looking at the renderer.

### Getting to a map, and finding out where it stopped

**`-autostart`** starts a new game with the premade character, no main menu, no
opening movie.  Everything about what the game costs while standing on a map
has to reach a map first, and reaching one by sending keystrokes cost five
minutes of movie per run and a menu that ignores the key for New Game while it
is fading in - three runs of twenty minutes each never got there.  With it, a
run reaches the map in about a minute of guest time.

**A stall line.**  The engine asks for input in every loop it has, so a line
printed from there says it is going round without drawing; silence says it is
inside a call that has not returned.  Which of the two it was is what decided
where to look next.

**A debugger.**  `ARGON_QEMU_EXTRA=-s` puts a gdb stub on port 1234, and
`build.py` keeps the intermediate ELF beside the `.AXE` so an address can be
turned into a function name:

```bash
xtensa-esp-elf-gdb -ex 'file build/argonos.elf' -ex 'add-symbol-file build/apps/FALLOUT2.elf -s .image 0x428c0000 -s .dimage 0x3c0e5d50'
```

The two addresses are in the loader's own line in the boot log.  gdb's own
register layout does not match this QEMU's, which is a nuisance and not a
blocker: the stub speaks a simple enough protocol that forty samples of the
program counter are twenty lines of Python, and `addr2line` does the rest.  A
day of guessing at a hang ended in ten minutes that way.

### Loading a map: 225 seconds, then 15

| | |
|---|---|
| `fread` with no buffer - `fgets` was one system call per character | −87% |
| `CONFIG_FATFS_USE_FASTSEEK` - a seek walked the cluster chain from the start | |
| the archive reopened for every entry inside it - dozens of opens a frame | −50% |

The first is in `apps/common/libc`, the second in `sdkconfig.defaults`, the
third in `apps/fallout/patches/fallout2-ce/0005-keep-the-archive-open.patch`.
The first two help every program on the system, not this one.

### Standing on a map: 835 ms a frame, then 158

Ninety-nine per cent of the file opens in an in-game frame **found nothing**:
56 per frame, 56 of them failures.  `master_patches` and `critter_patches`
name a directory the engine searches before the archive so that a loose file
can override one inside it; on a PC a lookup that fails is free, and on a FAT
card it is a directory walk.  They are empty now - see `fallout2.cfg`, which
says what that costs.

That is **6.2 frames a second** in QEMU, standing on the Temple of Trials,
against 0.03 when this started - measured in the configuration that would ship:
`-Psram 8`, a 6100 KB arena and a 1 MB art cache.  Sixteen megabytes with twice
the art cache measures 6.3, which is the same number and is the point of the
next paragraph.  The remaining frame is 162 ms, of which 96 ms is the
filesystem and 66 ms is the engine doing something nobody had looked at.

### Standing on a map: 162 ms a frame, then 40

Two more things, and neither of them was where anybody would have looked.

**The distance between two tiles was atan2 in software doubles, once per tile
stepped.**  Fallout asks how far one thing is from another constantly, and its
answer walks the hex grid one tile at a time, deciding at each step which of
six directions to take - with atan2, on doubles, on a chip that has neither.
A distance of thirty tiles was thirty trips through `__ieee754_atan2` and
friends.  With a debugger attached to the emulator, forty samples of the
program counter out of forty landed inside that chain: it was the whole of the
66 ms.  A table of tan(k degrees) and one 64-bit comparison per probe give the
same integer - see
`patches/fallout2-ce/0008-a-hex-distance-without-atan2.patch`, which also
explains why "the same" is safe.  The walk had no bound either, and an
unreachable target hung the machine; that is a bound now.

**The mute audio device did not take a second to play a second.**
`/dev/pcmnull` accepted whatever it was given at once and reported room for
ever, so the mixer ran as fast as the processor allowed and the game streamed
its music off the card six times faster than it played it: 1500 KB every three
seconds to produce 264 KB of sound, and 18-24 ms of every frame.  That is what
"it reads the card constantly while nothing moves" turned out to be.  The sink
keeps a clock now, in `components/argon_kernel/src/dev/audio.c`.

Standing still on the Temple of Trials, `-Psram 8`, over 210 three-second
windows - ten minutes of guest time, one long run rather than a chosen sample:

|                          | before | after |
|--------------------------|--------|-------|
| frame, median            | 162 ms | **37 ms** |
| frames a second, median  | 6.2    | **27.0** (mean 26.1, max 30.6) |
| windows under 10 fps     | all    | 1 of 210 |
| filesystem, per frame    | 96 ms  | 0.6-8.8 ms |
| read per three seconds   | 1500 KB | 60-500 KB |
| the engine, minus the filesystem | 66 ms | 10-22 ms |

Of the 37 ms, **24 is the display flush**, which has never been near the top
before.

### What the filesystem was asked for, and what the OS did about it

Two things went into the OS rather than the game, because both are the
filesystem being asked the wrong question rather than the game asking too
often.

**A lookup that finds nothing is remembered.**  On FAT it costs the same
directory walk as one that finds something, and a program that searches a
patch directory before an archive asks one for every sprite it draws.  The VFS
keeps a table of names that were not there; an entry marked as a directory
answers for everything below it, so one entry covers a whole missing tree - the
reason the table is thirty-two entries and not thousands.  `mount` prints how
many refusals came out of it.  See `components/argon_kernel/src/fs/vfs.c`; it
helps any program that looks in more than one place, which is most DOS-era
ones.

It does nothing for the frame rate quoted above, and that is not a
disappointment: `fallout2.cfg` empties the patch directories, so there are no
failed lookups left in the shipping configuration to make cheap.  What it buys
is that the workaround is no longer the only thing standing between the game
and 56 directory walks a frame - and putting the directories back is now a
question about memory rather than about speed.

**Read-ahead is the size of the read, and ramps.**  A fixed four kilobytes
throws three of them away when the caller takes a kilobyte out of the middle of
an archive and seeks elsewhere, which is exactly what reading one entry looks
like.  Each fill is now as big as the request and each fill that follows
another without a seek between may double, so reading a file front to back
still reaches four kilobytes in four fills.  `apps/common/libc/libc_shim.c`.

**This one has not been measured in the game.**  It is right on the arithmetic
and it passes the host tests, and that is all that can be said for it: the
three runs that were meant to measure it each ran into something else, and by
the time those were fixed the frame had changed shape enough that a comparison
against the old number would not have meant anything.  The change is a commit
of its own so that it can be taken back in one step.

### What is left

**The display flush, 24 ms of a 40 ms frame - and it is waiting, not working.**
`ag_port_panel_present` kicks QEMU's RGB device and, when the new rectangle
does not cover the one still in flight, waits for it.  That wait is the 24 ms:
the game now produces frames faster than the display takes them.  Which is a
good place to have arrived - the blit is 1.4 ms, the filesystem is under 9, and
what is left is the panel's own rate.

**The art cache still churns, but it no longer empties.**  Standing still, one
window in three fetches 20-80 entries and evicts 40-130; the other two fetch
nothing.  No whole-cache flushes at all now.  The largest single entry is
307 KB in a 1 MB cache, which is why one arrival can cost eighty evictions.

- **Not by reading ahead further.** Sixteen kilobytes instead of four was
  tried: two and a half times fewer calls, twice the time, 5.7 frames a second
  down to 3.3.  The cost is in the bytes, not the call.
- **Not the art cache's size.** 1 MB and 2 MB measured 6.2 and 6.3 frames a
  second before any of this; 2 MB and 4 MB measured the same as each other.
  Worth measuring again now that the frame has a different shape, but it has
  never yet been the answer.
- **Not the hex distance any more**: about 40 calls and 960 steps in a three
  second window, and no walk has yet hit its bound.

One thing on the OS side has *not* been tried: the reads land wherever the
archive entry is, so a fetch straddles sector boundaries at both ends and FatFs
serves those two ends through its own 512-byte per-file cache rather than as
part of the bulk transfer.  Rounding each fill down to a sector would make
every transfer a whole number of sectors for at most 511 wasted bytes.  Whether
that is worth anything depends on what a partial sector costs the SD layer,
which has not been measured.

### QEMU is pessimistic in both directions

It interprets Xtensa, so computation is slower than the chip; and its SD model
reads about 2.7 MB/s where a real card in four-bit mode does eight or more.
Both of the numbers above are floors for a board, not estimates of one, and
nothing here has been measured on hardware.

## Where the memory goes

**Fallout 2 fits in 8 MB of PSRAM.**  Measured, on `-Psram 8`, standing on a
loaded map with no allocation failure anywhere in the run:

| | |
|---|---|
| the image's own data, almost all bss | 1867 KB |
| the arena's high-water mark, transients included | 5567 KB |
| **needed** | **7434 KB** |
| free on a part with 8 MB of PSRAM | 8040 KB |
| **spare** | **606 KB** |

It needed 9910 KB when this started.  Four things took 2476 KB off it:

| | |
|---|---|
| the `.DAT` index read from flash instead of built in memory | −1841 KB |
| the panel set to 640×480, so the shim stops keeping its own copy of the frame | −608 KB |
| the map's ambience streamed instead of decoded whole | −488 KB |
| the music streaming ring, 24 buffers of 32 KB down to 8 of 16 | −640 KB |

Only two of those were work.  The panel was a line in a configuration file that
was on the card and not on `C:`, and it had been costing 600 KB for the whole
port.  The ring was nine seconds of audio held in advance, on a machine with
six megabytes.

### The ring came down twice, and the second time was not about audio

Eight sections of 32 KB is 256 KB, and the whole of it is asked for as one
block the moment a track starts.  The run that measured 6.2 frames a second
ended by asking for it: standing on the Temple of Trials, the map's ambience
changed, the arena had 499 KB free in no hole larger than 188 KB, and the
engine does not survive being refused a track - it gives up and exits.

Sixteen-kilobyte sections make it 128 KB, which is a hole the arena still has,
and a second and a half of audio held in advance rather than three.

The lesson is not about audio.  A quarter of a megabyte asked for *late*, on a
heap that has been in use for five thousand kilobytes, is a coin toss; this one
is a coin toss that ends the program.  `movie.cc` keeps a 200 KB copy of the
last frame of a movie for the fade out of it, and is the same bug waiting.

### The panel setting lives on `C:` and is worth 608 KB

`SYSTEM.CFG` sets the framebuffer to 640×480 so that the shim can hand the
engine the panel's own memory instead of keeping a copy of the frame.  It has
to be on `C:` - internal flash - and littlefs reformats itself without asking
whenever the partition does not mount.

When that happens the game does not say so.  It says `SDL: display 640x400`,
one line among fifty, and then dies four screens later asking for 256 KB it
would otherwise have had.  An evening went into blaming a filesystem change for
that.  `[mem]` after `art_init` tells them apart at a glance: 343 KB with the
panel, 951 KB without.

### How it was measured

`-memstat N` makes the port print the arena's use every N milliseconds, with
two high-water marks.  The first is what the game holds *between frames* - what
it costs to stand somewhere.  The second counts the big transients as well, and
that is the one the board has to be able to satisfy: an engine that reads a
file into a buffer, copies it and frees it again still needs the buffer to
exist.

Sampling once a frame never sees those, and that is not a detail.  Before the
ambience was streamed the two marks read 5610 and 6055 KB, and the 445 KB
between them is exactly what decided the question.  A watcher that only looks
between frames would have said the game fitted, and it did not.

With `-Psram 16` and a large arena, so that no ceiling is in the way:

| | between frames | with transients |
|---|---|---|
| through `gameInit` | 340 KB | 340 KB |
| sitting in the main menu | 5082 KB | 5311 KB |
| starting a new game, `elder.mve` playing | 5284 KB | 5386 KB |
| standing in the Temple of Trials | 5610 KB | 6055 KB |

Two things there are worth reading twice.

The map costs almost nothing over the menu: what the game holds is the art
cache and the protos, and it holds those whether or not a map is under them.

And the jump to 6055 was not the map either - it arrived with `wind2.acm`, the
Temple's ambience, which `map.cc` asked for with `SOUND_TYPE_MEMORY`: the whole
file decoded into the arena, 510528 bytes in one block.  When that failed there
were 548 KB free and the largest piece was 384 KB, so it was not short of
memory so much as short of *contiguous* memory - and giving the arena another
200 KB would have moved the wall rather than removed it.  Streaming it, which
is what the main menu already does with a track of the same kind off the same
card, costs the ring instead.

### Finding out who wants a block

`-memstat` also turns on a report of every allocation of half a megabyte or
more, with the address it was asked from:

```
malloc: 1049616 bytes from 0x4290b359
```

A failed allocation says how many bytes it wanted and nothing about who wanted
them, which in a vendored engine of 128000 lines is not a lead.  The loader
prints where the code was mapped (`code 762652 B XIP at 0x428c0000`); link with
`ARGON_FALLOUT_LDFLAGS=-Wl,-Map=build/fo2.map` and the difference is a function
name.  Even without one, the sizes and their order against the engine's own
progress printing usually name it - 1049616 is the art cache and nothing else
is.

### What it would take to have room

Two things are now measured that were not before.

**Two megabytes of art cache stops the tile thrash completely.**  Standing on
the Temple of Trials with `art_cache_size=2`, every three-second window reads
nothing, evicts nothing and flushes nothing - against one window in three
fetching 20-80 entries and evicting 40-130 at 1 MB.  Four megabytes is not
needed; two is the whole answer, and it is what makes the game feel smooth
rather than merely fast.

**A map change wants 837 KB more than standing still.**  The arena peaks at
5254 KB on a loaded map (1 MB cache, transients included).  Entering the Temple
asked for 32 KB when 41 KB was free in no hole larger than 31 KB - so the real
demand is 6091 KB against an arena of 6100, and the engine exits when it is
refused.

And the arena cannot be made bigger.  8007 KB of extended memory is free with
the card mounted and the image's data takes 1812, but the arena has to be one
block: 6150 is refused, and so are 6220 and 6360.  All three were tried.

**And it does not take two megabytes.**  The setting is whole megabytes, which
is far too coarse a dial here, so patch 0012 reads it in kilobytes above 16.
1400 KB measures exactly like 2048 - nothing read, nothing evicted, nothing
flushed - and costs 354 KB of arena rather than 1056.  That is the shipping
value now.  The knee is somewhere between 1024 and 1400 and has not been
narrowed further, because 1400 already leaves margin and each measurement is a
ten-minute run.

So the whole budget, on an 8 MB part:

| | KB |
|---|---|
| image data, almost all bss | 1812 |
| arena, standing still, 1400 KB cache and the smaller ring | 5542 |
| a map change, on top of that | 837 |
| **wanted** | **8191** |
| the part has | 8007 |
| **short by** | **184** |

Which is a different problem from the 920 KB it looked like.  Measured in the
configuration that ships: `-Psram 8`, arena 6100, cache 1400 KB, music ring
4x16 KB - 27 frames a second, 558 KB of arena free at the high-water mark, and
nothing read or evicted by the art cache once the map is up.  Where it can come
from, largest first:

| KB | done | |
|---|---|---|
| 320 | no | `gTileIntensity` is `int[3][40000]` in bss - three map levels of 200×200 hexes. Almost every map has one level. Allocating the rows a map actually touches moves 480 KB of bss into 160 KB of arena. Narrowing the type is the other half of the same 320 and can be had as well: the values are added and subtracted and pass 65536 before being clamped on read, so they do not fit `uint16` as they are - but stored shifted down two bits they do, and two bits of a lighting value that ends up as one of sixty-four palette shades is not visible. Three lights overlapping one hex would then saturate |
| 128 | no | four 32 KB zlib windows, one per open archive stream. Patch 0005 keeps the archive open; how many streams are live at once has not been looked at |
| 80 | no | `_square_data`, the same three-levels-for-one trick |
| 64 | **yes** | the music streaming ring, four sections of 16 KB rather than eight |

The first line alone covers it.  What will not help: the three 640×480 8-bit
buffers (900 KB between them) are the interface window's, the splash screen's -
freed after startup - and a scratch buffer in `_GNW_win_refresh` that patch 0011
does away with. And `_screen_buffer`, which looked dead and *is* dead, was
never allocated: the flag `gameInitWithOptions` passes is zero.

### The framebuffer is free here and will not be on a board

The panel's 640×480 RGB565 buffer is 600 KB, and it does not come out of the
8007 KB above: `esp_lcd_rgb_qemu` hands over memory the emulated device owns.
On a real panel that buffer is PSRAM, so every figure here is 600 KB optimistic
for hardware - which would put a map change 850 KB out of reach rather than
250.  Halving the panel to 320×240 would give 450 KB of it back and is what the
CYD would need anyway; the engine would keep drawing 640×480 into its own
8-bit surface and the port's blit would take every second pixel.  Nothing about
this has been measured on hardware.

### What is left in the arena

| | |
|---|---|
| art cache (`art_cache_size`, minimum 1) | 1 MB |
| the background sound streaming ring | 256 KB |
| sound cache (`cache_size`) | 64 KB |
| the 8-bit surface the game draws into | 300 KB |
| protos, scripts, message files, the rest | the remainder |

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

### What is left, and what it costs

- **97 KB, a build flag**: `--rodata-in-code`, above.  Buys memory with cache
  pressure; not needed as things stand.
- **240 KB, and it does not work**: `gTileIntensity` as `uint16_t`.  Light is
  clamped to 65536 *on read*, and the stored value is a running sum:
  `lightIncreaseTileIntensity` adds a source and `lightDecreaseTileIntensity`
  takes it away again, so two lamps on one tile hold 131072 between them and
  `lightGetTrueTileIntensity` hands that back unclamped.  Saturating at 65535
  would be invisible until a light moved away, at which point the subtraction
  would take a tile dark that should still be lit.  The array has to be as wide
  as the sum, not as the answer.
- **900 KB, when the machine is idle**: the attract loop.  `mainmenu.cc` waits
  `gMainMenuScreensaverDelay` and plays `intro.mve`, whose player costs more
  than anything the game does while it is being played.  Untested against the
  606 KB above, which means the screensaver may not fit even though the game
  does.

### The caveats, plainly

The 606 KB is spare on **one map**, in QEMU.  Three things are not in it:

- **The panel.** QEMU lends the emulated display's own memory for the
  framebuffer; a real 640×480 panel takes its 600 KB out of the same PSRAM,
  which is very nearly all of the margin.  No cheap SPI panel is 640×480
  anyway, so that is a different question from this one.
- **The rest of the game.** One map has been stood on.  A city with more
  critters, a longer ambience, a fight, an inventory screen - none of it has
  been measured.
- **The attract movie**, above.

### The read-only part moves to flash

ArgonOS can put a file at an address instead of in memory: `ag_map` stages it
into the `appfs` partition and maps it into the data window, where the cache
that fetches instructions fetches it too. It costs no RAM at all. That is what
`components/argon_kernel/src/fs/filemap.c` exists for, and it changes the
arithmetic - for the one item big enough to matter.

**The `.DAT` index.** 30240 entries of path, offset and size, built into the
arena at every start by reading the archive - about 150000 small reads and ten
seconds. For a given `MASTER.DAT` those bytes never change, so they are
computed once on the host and read where they lie:

```
python apps/fallout/tools/mkdatidx.py build/fo2_card/master.dat build/fo2_card/critter.dat
```

```
master.dat -> master.IDX: 23120 entries, 1012 KB (1373 KB in the arena otherwise)
critter.dat -> critter.IDX: 7120 entries, 319 KB (431 KB in the arena otherwise)
```

The file is written in the layout the engine wants to read, not one that is
convenient to write: the entry table is the `DBaseEntry` array verbatim, so the
binary search runs over those bytes with nothing unpacked and nothing copied,
and every offset counts from the start of the file, so it does not matter where
the mapping lands. `dbaseOpen` looks for `<name>.IDX` beside the archive; the
header carries the `.DAT`'s size, and an index that does not match its archive
is refused with a line saying so rather than believed - a stale index describes
files at offsets that are no longer there, and every read would come back as
plausible rubbish.

Without an `.IDX` the engine builds the table as before, but into **two**
allocations rather than 30241: the table says exactly how many bytes all the
paths need, so they go in one block, and a quarter of what the old way spent
was allocator headers. It costs nothing to have and it is what happens on a
card with no `.IDX` on it.

`apps/fallout/port/dat_index.{h,cc}` is the ArgonOS half; the engine side is
twelve lines in `apps/fallout/patches/fallout2-ce/0001-dat-index-from-flash.patch`.
Staging into flash is paid once per boot: 1331 KB of it, a few seconds, and
`appfs` is 4032 KB against the 745 KB of XIP code `FALLOUT2.AXE` already puts
there.

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
