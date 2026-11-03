# Mega Drive (`MD.AXE`) — Z80 + approximate sound

68000, VDP, Z80, PSG, and a light YM2612→`ag_fm` front-end. Video goes straight
into the `gfx` back buffer. Sound: `/dev/pcmnull` (default), `/dev/pcmvirt`
(after `drv install` of [`PCMVIRT.SYS`](../pcmvirt)), WAV file, or future `/dev/pcm0`.

Cores: [gwenesis](https://github.com/bzhxx/gwenesis) by bzhxx — read
**Licensing** below before shipping this anywhere.

## Licensing

This is the one part of the app that cannot be figured out from the code, so it
is written down here.

| Component | Licence | Notes |
|---|---|---|
| `core/vdp`, `core/bus`, `core/io`, `core/savestate` | GPLv3 | gwenesis' own files say GPLv3; the repository's `LICENSE` says AGPL-3.0. Vendored as `LICENSE` |
| `core/cpus/M68K` | MIT | Musashi by Karl Stenerud. See below |
| Z80 | BSD-3-Clause | linked from `apps/sms/core/z80` (MAME), not Fayzullin |
| PSG | GPLv2+ | SMS `sn76489.c` (CrabEmu) behind gwenesis SN76489 names |
| `ag_fm` | Apache-2.0 | `apps/common/fm` — OPN 4-op + sin/TL lite FM, default `MD.AXE` |
| `core/sound/ym2612.c` | GPLv3 (MAME/Genesis Plus GX lineage) | only in comparison build `MDYM.AXE` |
| `md_main.c`, `md_cfg.h`, `port/` | Apache-2.0 | ours |
| Z80 (`Z80.c` by Marat Fayzullin) | **not vendored** | commercial distribution forbidden |
| `gwenesis_sn76489.c` | **not compiled** | PSG is SMS `sn76489.c` instead |

Two things to know about the two CPU cores gwenesis bundles.

The M68K core is Musashi, and the copy inside gwenesis carries the 1998–2001
readme, which grants use "free for any non-commercial purpose" and asks you to
contact the author for anything else. Karl Stenerud has since released Musashi
under the MIT licence, and that grant covers the same code; the current upstream
text is vendored beside the sources as `MUSASHI-MIT-readme.txt`. gwenesis'
modifications to it (pre-generated opcode and cycle tables specialised for a
microcontroller) are bzhxx's and GPLv3, so the directory as a whole is GPLv3 with
an MIT core inside it. Note that the files here are *not* interchangeable with
upstream Musashi: upstream generates `m68kops` from `m68k_in.c` at build time and
its `m68kcpu.c` is 54 KB against 15 KB here.

The Z80 gwenesis uses has no such history: Marat Fayzullin's licence forbids
commercial distribution outright, and there is no later grant. It is therefore
not in this tree at all. Sound uses the BSD-3 Z80 already vendored for the Master
System (`apps/sms/core/z80/z80.c`) behind the same `z80inst.h` interface.

## Local changes to the vendored core

Marked here because GPLv3 asks for it, and because the next person to re-vendor
gwenesis needs the list.

- `vdp/gwenesis_vdp_gfx.c` — the RGB565 rendering path is gated on
  `GNW_TARGET_MARIO || GNW_TARGET_ZELDA` upstream; it now also accepts
  `ARGON_TARGET`. Added `screen_buffer_stride` and
  `gwenesis_vdp_set_buffer_stride()`, because upstream steps exactly
  `SCREEN_WIDTH` pixels per line, which would force the framebuffer to be 320
  wide and cost a full-frame copy per frame. `Ofast` also under `ARGON_TARGET`
- `vdp/gwenesis_vdp_mem.c` — `Ofast` under `ARGON_TARGET`
- `bus/gwenesis_bus.c`, `cpus/M68K/m68k.h` — under `ARGON_TARGET`, `ROM_DATA` is
  a pointer to the image the app already loaded instead of a static
  `unsigned char[8 MB]`, and `load_cartridge()` takes ownership of that buffer
  and byte-swaps it in place rather than copying into the static array. Upstream
  reserves 8 MB of `.bss` for a cartridge that is usually 512 KB
- `cpus/M68K/m68kcpu.c` — `m68k_frame_end()`, `m68k_arm_address_error_trap()` /
  `m68k_on_address_error()` so the platform can arm `setjmp` once per frame
- `cpus/M68K/m68kcpu.h` — under `ARGON_TARGET`, `m68ki_set_address_error_trap()`
  is empty (trap armed from `md_main.c`, not 262×/frame inside `m68k_run`)
- deleted `m68ki_cycles_full.h` and `m68ki_instruction_jump_table_full.h`
  (1.7 MB of source for the `TABLES_FULL` build, which we do not use)

SMS Z80 (`apps/sms/core/z80/z80.c`): under `ARGON_MD_Z80`, skip SMS `shared.h`,
use `argon_z80_read`/`write` for the MD map, and rename the relative-cycle
`z80_execute` to `sms_z80_execute` so it does not clash with `z80inst.h`.

`sound/ym2612.c` (only linked into `MDYM.AXE`): `Ofast` and 22050 Hz sample
divisor under `ARGON_TARGET`; buffer clamp; host-generated `tl`/`sin`/LFO tables
(`ym2612_tables_argon.inc`) instead of `libm` at init; save-state helpers live
in `port/md_ym_native.c`.

## Build

```
python tools/mkaxe.py --arch xtensa --gcc xtensa-esp32s3-elf-gcc `
  --include sdk/include --include apps/common --include apps/common/libc --include apps/md `
  --include apps/md/port --include apps/md/core/bus --include apps/md/core/cpus/M68K `
  --include apps/md/core/vdp --include apps/md/core/io `
  --include apps/md/core/savestate --include apps/md/core/sound `
  --include apps/sms/core/z80 --include apps/sms/core/sound --include apps/common/fm --include apps/common/dsp `
  --cflags "-Os -ffunction-sections -fdata-sections -fno-builtin -include md_cfg.h -DARGON_MD_Z80 -DLSB_FIRST -Wno-unused -Wno-sign-compare" `
  --libs c -o build/apps/MD.AXE `
  apps/md/md_main.c `
  apps/md/port/md_z80.c apps/md/port/md_ym_ag_fm.c apps/md/port/md_psg.c `
  apps/md/port/md_sound_out.c apps/md/port/md_savestate.c `
  apps/common/libc/libc_shim.c apps/common/fm/ag_fm.c apps/common/dsp/ag_dsp.c `
  apps/sms/core/z80/z80.c apps/sms/core/sound/sn76489.c `
  apps/md/core/bus/gwenesis_bus.c apps/md/core/cpus/M68K/m68kcpu.c `
  apps/md/core/vdp/gwenesis_vdp_gfx.c apps/md/core/vdp/gwenesis_vdp_mem.c `
  apps/md/core/io/gwenesis_io.c apps/md/core/savestate/gwenesis_savestate.c
```

`-include md_cfg.h` is required: it is how the vendored core learns it is being
built for ArgonOS (see `md_cfg.h`). `-DARGON_MD_Z80` selects the MD memory path
in the shared SMS Z80. `--libs c` is for `setjmp` (68000 address error).

### Native YM2612 comparison (`MDYM.AXE`)

Same app, but FM comes from vendored `ym2612.c` instead of `ag_fm`. Use this to
A/B listen over `net` / `pcmplay.py`. Sin/pow tables are host-generated
(`tools/gen_ym2612_tables.py` → `ym2612_tables_argon.inc`) so we do not need
`libm`. Heavier CPU — expect lower % realtime.

```
python tools/mkaxe.py --arch xtensa --gcc xtensa-esp32s3-elf-gcc `
  --include sdk/include --include apps/common --include apps/common/libc --include apps/md `
  --include apps/md/port --include apps/md/core/bus --include apps/md/core/cpus/M68K `
  --include apps/md/core/vdp --include apps/md/core/io `
  --include apps/md/core/savestate --include apps/md/core/sound `
  --include apps/sms/core/z80 --include apps/sms/core/sound `
  --cflags "-Os -ffunction-sections -fdata-sections -fno-builtin -include md_cfg.h -DARGON_MD_Z80 -DLSB_FIRST -Wno-unused -Wno-sign-compare" `
  --libs c -o build/apps/MDYM.AXE `
  apps/md/md_main.c `
  apps/md/port/md_z80.c apps/md/port/md_ym_native.c apps/md/port/md_psg.c `
  apps/md/port/md_sound_out.c apps/md/port/md_snd_async.c apps/md/port/md_raster.c `
  apps/md/port/md_savestate.c apps/common/libc/libc_shim.c `
  apps/sms/core/z80/z80.c apps/sms/core/sound/sn76489.c `
  apps/md/core/sound/ym2612.c `
  apps/md/core/bus/gwenesis_bus.c apps/md/core/cpus/M68K/m68kcpu.c `
  apps/md/core/vdp/gwenesis_vdp_gfx.c apps/md/core/vdp/gwenesis_vdp_mem.c `
  apps/md/core/io/gwenesis_io.c apps/md/core/savestate/gwenesis_savestate.c
```

Both land in `build/sd_card/` via staging. Compare:

```
run h:\MD.AXE h:\game.bin pcmvirt      # lite FM
run h:\MDYM.AXE h:\game.bin pcmvirt    # native YM2612
```

The image is **~170 KB of code** (`MD.AXE`) in the 192 KB arena and asks for a
**24 KB** stack (OpenEth/lwIP leave a ~31 KB largest internal free block). Cycle
tables stay in ordinary `.rodata` (PSRAM) so the Z80 fits; put them back in
`AG_HOT_RODATA` only if the arena grows.

### The CYD (`MD-NO-PSRAM.AXE`: ESP32, no PSRAM)

`ARGON_MD_SMALL`: the machine's memory in 512-byte pages, VRAM partly in IRAM
(word access only), the cartridge staged in flash, tables in flash read by
words, and the image split (`MD_SPLIT`, `--split-hot`): the 68000's loop, the
handlers a game runs (`md_hot.h`) and the YM2612's synthesis loop in the hot
part, the rest streamed into flash. Sound is the native YM2612 with its tables
at 1 KB (`tools/gen_ym2612_tables.py`) and the PSG, mono, to `/dev/pcm0`, the
chips at 11 025 Hz (`hifi`: 22 050). Run from the card, then give the card back
so the machine's memory fits: `run a:\md-no-psram.axe a:\game.swp waitcard`,
Ctrl+\, `eject`, Alt+1.

The firmware under it is `argon target esp32-cyd-net` (the CYD with Wi-Fi):
on 1 Nov 2026 the machine started five times in a row there and ran at the
same speed as with `esp32-cyd-md`. That target adds `sdkconfig.esp32.cydmd` -
the radio's PHY paths in flash, 9 KB more IRAM - as a margin for a start that
comes up a page short. The monitor build, `esp32-cyd`, has no radio in the
image at all.

Raster effects (`md_raster.c`): the frame's mid-screen writes to the VDP's
registers, vertical scroll and palette are recorded with their lines, and the
horizontal scroll table is copied before the vertical blank rewrites it (and
before any write into it while lines are shown), so a pass drawn afterwards
by both cores gives each band its own scroll. The record takes what the heap
leaves after the machine's pages; `stats` says how many writes it holds and
how many frames had more.

```
. .\tools\idf-env.ps1
python tools/mkaxe.py --arch xtensa --gcc xtensa-esp32-elf-gcc --no-stage --keep-elf build/apps/MD-NO-PSRAM.AXE.elf `
  --include sdk/include --include apps/common --include apps/common/libc --include apps/md `
  --include apps/md/port --include apps/md/core/bus --include apps/md/core/cpus/M68K `
  --include apps/md/core/vdp --include apps/md/core/io `
  --include apps/md/core/savestate --include apps/md/core/sound `
  --include apps/sms/core/z80 --include apps/sms/core/sound --include apps/common/fm --include apps/common/dsp `
  --cflags "-Os -DMD_SPLIT -ffunction-sections -fdata-sections -fno-builtin -include md_cfg.h -DARGON_MD_Z80 -DARGON_MD_SMALL -DARGON_SMS_SMALL -DLSB_FIRST -Wno-unused -Wno-sign-compare" `
  --code-rodata .rodata.op_cb --code-rodata .rodata.op_dd --code-rodata .rodata.op_ed --code-rodata .rodata.op_fd `
  --code-rodata .rodata.dd_cb --code-rodata .rodata.fd_cb --code-rodata .rodata.sms_z80_execute --code-rodata .rodata.k_font8x16 `
  --libs c --split-hot -o build/apps/MD-NO-PSRAM.AXE `
  apps/md/md_main.c `
  apps/md/port/md_z80.c apps/md/port/md_ym_native.c apps/md/core/sound/ym2612.c apps/md/port/md_psg.c `
  apps/md/port/md_sound_out.c apps/md/port/md_snd_async.c apps/md/port/md_raster.c `
  apps/md/port/md_savestate.c apps/common/libc/libc_shim.c `
  apps/sms/core/z80/z80.c apps/sms/core/sound/sn76489.c `
  apps/md/core/bus/gwenesis_bus.c apps/md/core/cpus/M68K/m68kcpu.c `
  apps/md/core/vdp/gwenesis_vdp_gfx.c apps/md/core/vdp/gwenesis_vdp_mem.c `
  apps/md/core/io/gwenesis_io.c apps/md/core/savestate/gwenesis_savestate.c
```

### The S3-CAM (`MDS3.AXE`: the CYD's pipeline on an ESP32-S3 with PSRAM)

The same build as the CYD's (`ARGON_MD_SMALL`, `MD_SPLIT`: rows to the SPI
panel, both cores drawing, the Z80 and the chips on the system core) with
`xtensa-esp32s3-elf-gcc` and `-DMD_PSRAM_PAGES`: the machine's pages that
internal memory cannot give come from the process's arena, which is PSRAM
there (the image asks for 192 KB). The ordinary `MD.AXE` is 790 KB and the
S3-CAM's C: is 1 MB with no card in the slot, so this is the one that fits
beside a ROM. Measured 1 Nov 2026 with Alex Kidd from C: on the ST7789:
59 fps, 30-58 whole pictures a second. Not yet: the raster record (a record
in PSRAM stopped the 68000's loop), and the system shell gets little time
while it runs.

```
run c:\mds3.axe c:\alex.swp 0 stats
```

## Run

```
argon run -Gfx -HostFs build\sd_card
run h:\MD.AXE h:\game.bin
run h:\MD.AXE h:\game.bin mock        # Z80+PSG+FM → /dev/pcmnull (default)
run h:\MD.AXE h:\game.bin wav         # write wav on a mounted drive
run h:\MD.AXE h:\game.bin pcmvirt     # /dev/pcmvirt → Windows (see below)
```

Realtime audio (OpenEth NIC, not UART) — full steps in
[`apps/pcmvirt/README.md`](../pcmvirt/README.md):

```
argon run -Gfx -HostFs build\sd_card
# guest: drv install h:\pcmvirt.sys ; run h:\MD.AXE h:\game.bin pcmvirt
python tools/pcmplay.py                 # or: python tools/pcmplay.py --ffplay
python tools/pcmplay.py --record build\md_agfm.wav   # play + save
python tools/pcmplay.py --save build\md_only.wav     # save, no speakers
```

`argon run` hostfwd’s `127.0.0.1:5558` by default.

| Arg | Effect |
|-----|--------|
| path | cartridge image, up to 4 MB, any extension. Without one, a synthetic 1 KB ROM that just spins — for benchmarking the VDP with an idle CPU |
| N (digits) | run N frames and exit, for benches |
| `stats` | print fps, per-frame split (emu / show), max, and **% realtime** every 2 s and at exit |
| `fps30` | emulate all 60 frames, rasterise and show every second one |
| `fps60` | show every frame (default) |
| `livepad` / `nolivepad` | live pad via `inp->btnp` (HostFS PADPUSH), or serial sticky only |
| `mock` / `wav` / path | sound sink: discard, `a:\md.wav`, or an explicit path |
| `net` / `net:PORT` | stream stereo s16 PCM over TCP (WAV header); host: `tools/pcmplay.py` |
| `audio` / `i2s` | kernel `api->audio` — real I2S when `[audio]` pins are set, else stub |
| `profile` | print m68k/z80/snd/vdp µs for the first 120 frames |
| `noz80` | advance Z80 clock only (no CPU execute; BUSREQ still live) |
| `nosound` | skip sample mix |

Startup frames on Alex Kidd are slow because the **68000** is busy (~35 ms/frame
of m68k time), not because something waits on the Z80 — confirmed with
`profile` vs `noz80 nosound`. It settles on its own after a couple of seconds.

Keys (with `-HostFs` and `sms.cfg`): arrows, `Z` = A, `X` = B, `C` = C,
Enter = Start, Esc quits. That path is real level state from a Win32 keyboard
hook, so directions and buttons can be held. `nolivepad` forces serial sticky
keys: the terminal has no key-up, so a held button expires quickly — a degraded
reserve, not for play.

Save states are accepted and silently discarded — the cores can serialise
themselves, but nothing writes the bytes yet.

The image has to be a **raw** cartridge dump, whatever it is called: a 68000
vector table at 0 and `SEGA` in the header. Interleaved Super Magic Drive files
are not supported.

Default `MD.AXE` YM is lite `ag_fm` OPN: 4 operators, ALG 0–7, FB, sin/TL
tables (`ag_fm_ym_tables.inc` from `tools/gen_ym2612_tables.py`), simplified
EG (no DT/LFO/SSG), pitch matched to `MDYM.AXE` by ear, −6 dB mix pad.
`MDYM.AXE` remains the chip-accurate A/B reference (`net` +
`pcmplay.py --record`). FM drums should stay percussive rather than a squeak;
DAC/PCM on ch6 is a separate path. PSG matches SMS.

### The game window, the pause menu and the phone pad (3 Nov 2026)

- Started with no cartridge (`run a:\md.axe`), MD shows its own window on
  the glass: the card's folders and cartridges (or the drives), A/C/START
  to open, B for the folder above, MENU to leave. A cartridge is a `.SWP` or
  a plain `.BIN`/`.MD`/`.GEN`: a dump with "SEGA" at 0x100 is swapped into a
  `.SWP` beside it the first time, one already swapped ("ESAG") plays as it
  is. Nothing is held while the window is up but a row of the ring, and the
  listing is read from the folder for each row drawn.
- On the CYD the cartridge is staged into flash and the card let go
  (`ag_media(false)`, ABI 0.77) before the machine takes its memory; LOAD
  GAME gives all of it back (`md_pages_free`, `md_raster_free`, the map) and
  mounts the card again for the window.
- Esc (the phone pad's MENU, a keyboard's Esc, a pad's quit) pauses the game:
  RESUME / SAVE STATE / LOAD STATE / LOAD GAME / EXIT. A state is
  PackBits-compressed (Alex Kidd: 36 KB of 144, md_savestate.c), beside the
  cartridge on a PSRAM board and in `C:\` on the CYD, whose card is out while
  it plays. Its file is opened when the game starts and kept: mid-game the
  CYD has no 512 bytes in one piece for littlefs's buffer. The front end's
  font is in flash (`--code-rodata .rodata.k_font8x16`), read by words.
- The pad: `drv load c:\phone.sys`, then `http://<board>:8765/#pad` on the
  phone (or the 🎮 button on the page). The page asks the board for no
  picture (`IN_PADONLY`), and its buttons go as keys - arrows, Z X C for
  A B C, Enter for Start, Esc for MENU - which the pad layer already reads.
  Not on the CYD while MD runs: Wi-Fi costs 30 KB and PHONE.SYS 20 more,
  and the machine leaves a couple of kilobytes.
