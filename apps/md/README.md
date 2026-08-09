# Mega Drive (`MD.AXE`) — mute milestone

68000 and VDP only. No sound: the Z80, YM2612 and PSG are stubbed in
`port/md_mute.c` and are not compiled in. Video goes straight into the `gfx`
back buffer, keyboard or host pad drive controller 1.

Cores: [gwenesis](https://github.com/bzhxx/gwenesis) by bzhxx — read
**Licensing** below before shipping this anywhere.

## Licensing

This is the one part of the app that cannot be figured out from the code, so it
is written down here.

| Component | Licence | Notes |
|---|---|---|
| `core/vdp`, `core/bus`, `core/io`, `core/savestate` | GPLv3 | gwenesis' own files say GPLv3; the repository's `LICENSE` says AGPL-3.0. Vendored as `LICENSE` |
| `core/cpus/M68K` | MIT | Musashi by Karl Stenerud. See below |
| `md_main.c`, `md_cfg.h`, `port/` | Apache-2.0 | ours |
| Z80 (`Z80.c` by Marat Fayzullin) | **not vendored** | "You are not allowed to distribute this software commercially" |
| YM2612, SN76489 | **not compiled** | present under `core/sound` for the sound milestone |

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
not in this tree at all. The mute milestone does not need it, and when the sound
block lands it will use the GPLv2+ Z80 already vendored for the Master System
(`apps/sms/core/z80/z80.c`) behind the same `z80inst.h` interface.

## Local changes to the vendored core

Marked here because GPLv3 asks for it, and because the next person to re-vendor
gwenesis needs the list.

- `vdp/gwenesis_vdp_gfx.c` — the RGB565 rendering path is gated on
  `GNW_TARGET_MARIO || GNW_TARGET_ZELDA` upstream; it now also accepts
  `ARGON_TARGET`. Added `screen_buffer_stride` and
  `gwenesis_vdp_set_buffer_stride()`, because upstream steps exactly
  `SCREEN_WIDTH` pixels per line, which would force the framebuffer to be 320
  wide and cost a full-frame copy per frame
- `bus/gwenesis_bus.c`, `cpus/M68K/m68k.h` — under `ARGON_TARGET`, `ROM_DATA` is
  a pointer to the image the app already loaded instead of a static
  `unsigned char[8 MB]`, and `load_cartridge()` takes ownership of that buffer
  and byte-swaps it in place rather than copying into the static array. Upstream
  reserves 8 MB of `.bss` for a cartridge that is usually 512 KB
- `cpus/M68K/m68kcpu.c` — added `m68k_frame_end()`, which rebases the CPU cycle
  counter at end of frame. Upstream platforms reach into `m68k.cycles` directly;
  that global is file-static, and the platform has no business including the
  55 KB `m68kcpu.h`
- deleted `m68ki_cycles_full.h` and `m68ki_instruction_jump_table_full.h`
  (1.7 MB of source for the `TABLES_FULL` build, which we do not use)

## Build

```
python tools/mkaxe.py --arch xtensa --gcc xtensa-esp32s3-elf-gcc `
  --include sdk/include --include apps/common/libc --include apps/md `
  --include apps/md/core/bus --include apps/md/core/cpus/M68K `
  --include apps/md/core/vdp --include apps/md/core/io `
  --include apps/md/core/savestate --include apps/md/core/sound `
  --cflags "-Os -ffunction-sections -fdata-sections -fno-builtin -include md_cfg.h -Wno-unused -Wno-sign-compare" `
  --libs c -o build/MD.AXE `
  apps/md/md_main.c apps/md/port/md_mute.c apps/common/libc/libc_shim.c `
  apps/md/core/bus/gwenesis_bus.c apps/md/core/cpus/M68K/m68kcpu.c `
  apps/md/core/vdp/gwenesis_vdp_gfx.c apps/md/core/vdp/gwenesis_vdp_mem.c `
  apps/md/core/io/gwenesis_io.c apps/md/core/savestate/gwenesis_savestate.c
```

`-include md_cfg.h` is required: it is how the vendored core learns it is being
built for ArgonOS (see `md_cfg.h`).

`--libs c` is for `setjmp` alone. Musashi uses it to unwind a 68000 address error
— an unaligned access by the game — and that check is what stops an odd guest
address from becoming an unaligned load on the host, where it would fault. So
`M68K_EMULATE_ADDRESS_ERROR` stays on and one object comes out of newlib.

The image is **129 KB of code**, which does not fit the 64 KB arena; ArgonOS is
built with `CONFIG_ARGON_APP_ARENA_KB=192` so that it lands in internal SRAM
whole. There are also **61 643 relocations**, nearly all of them entries in the
68000's instruction jump table, so expect the loader to spend real time on them.

## Run

```
argon sync build\sms_share          # with MD.AXE copied in
argon run -Sd -Gfx
run a:\MD.AXE a:\game.bin
```

| Arg | Effect |
|-----|--------|
| path | cartridge image, up to 4 MB, any extension. Without one, a synthetic 1 KB ROM that just spins — for benchmarking the VDP with an idle CPU |
| N (digits) | run N frames and exit, for benches |
| `stats` | print fps, per-frame split (emu / show), max, and **% realtime** every 2 s and at exit |
| `fps30` | emulate all 60 frames, rasterise and show every second one |
| `fps60` | show every frame (default) |
| `livepad` / `nolivepad` | use `H:\sms.pad` host push, or serial keys only |

Keys: arrows, `Z` = A, `X` = B, `C` = C, Enter = Start, Esc or `Q` quits. Serial
has no key-up, so a held button expires after a second. The host pad is the file
hostfsd already serves (`/sms.pad`); its two buttons map to A and B and its pause
bit to Start.

Save states are accepted and silently discarded — the cores can serialise
themselves, but nothing writes the bytes yet.

The image has to be a **raw** cartridge dump, whatever it is called: a 68000
vector table at offset 0 and `SEGA MEGA DRIVE` or `SEGA GENESIS` at 0x100. Files
in the Super Magic Drive format — 512-byte header, then 16 KB blocks with the
even and odd bytes separated — are not deinterleaved here and will not run, so
check the size before blaming the emulator: an SMD file's length is 512 plus a
multiple of 16384. A `.smd` extension on a file whose length is a clean multiple
of 16 KB is just a misnamed raw dump and works fine.

## Speed

```
argon test -Sd -TimeoutSec 240 "run a:\MD.AXE 300 nolivepad stats" "errorlevel"
```

`% realtime` is against one NTSC frame of 16667 µs, so 100% means exactly keeping
up. Numbers and what they do and do not prove:
[`docs/07-emulator-performance.md`](../../docs/07-emulator-performance.md).
