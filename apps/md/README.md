# Mega Drive (`MD.AXE`) — Z80 + approximate sound

68000, VDP, Z80, PSG, and a light YM2612→`ag_fm` front-end. Video goes straight
into the `gfx` back buffer. Sound is WAV or mock until the kernel has `audio`/I2S.

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
| `ag_fm` | Apache-2.0 | `apps/common/fm` — approximate FM, not a real YM2612 |
| `md_main.c`, `md_cfg.h`, `port/` | Apache-2.0 | ours |
| Z80 (`Z80.c` by Marat Fayzullin) | **not vendored** | commercial distribution forbidden |
| `core/sound/ym2612.c`, `gwenesis_sn76489.c` | **not compiled** | present for reference / fallback only |

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

## Build

```
python tools/mkaxe.py --arch xtensa --gcc xtensa-esp32s3-elf-gcc `
  --include sdk/include --include apps/common/libc --include apps/md `
  --include apps/md/port --include apps/md/core/bus --include apps/md/core/cpus/M68K `
  --include apps/md/core/vdp --include apps/md/core/io `
  --include apps/md/core/savestate --include apps/md/core/sound `
  --include apps/sms/core/z80 --include apps/sms/core/sound --include apps/common/fm `
  --cflags "-Os -ffunction-sections -fdata-sections -fno-builtin -include md_cfg.h -DARGON_MD_Z80 -DLSB_FIRST -Wno-unused -Wno-sign-compare" `
  --libs c -o build/MD.AXE `
  apps/md/md_main.c `
  apps/md/port/md_z80.c apps/md/port/md_ym_ag_fm.c apps/md/port/md_psg.c `
  apps/md/port/md_sound_out.c apps/md/port/md_savestate_stub.c `
  apps/common/libc/libc_shim.c apps/common/fm/ag_fm.c `
  apps/sms/core/z80/z80.c apps/sms/core/sound/sn76489.c `
  apps/md/core/bus/gwenesis_bus.c apps/md/core/cpus/M68K/m68kcpu.c `
  apps/md/core/vdp/gwenesis_vdp_gfx.c apps/md/core/vdp/gwenesis_vdp_mem.c `
  apps/md/core/io/gwenesis_io.c apps/md/core/savestate/gwenesis_savestate.c
```

`-include md_cfg.h` is required: it is how the vendored core learns it is being
built for ArgonOS (see `md_cfg.h`). `-DARGON_MD_Z80` selects the MD memory path
in the shared SMS Z80. `--libs c` is for `setjmp` (68000 address error).

The image is **~170 KB of code** in the 192 KB arena and asks for a **24 KB**
stack (OpenEth/lwIP leave a ~31 KB largest internal free block). Cycle tables
stay in ordinary `.rodata` (PSRAM) so the Z80 fits; put them back in
`AG_HOT_RODATA` only if the arena grows.

## Run

```
argon sync build\sd_card          # with MD.AXE copied in
argon run -Sd -Gfx
run a:\MD.AXE a:\game.bin
run a:\MD.AXE a:\game.bin mock        # Z80+PSG+FM, discard samples (default)
run a:\MD.AXE a:\game.bin wav         # write a:\md.wav
run a:\MD.AXE a:\game.bin a:\out.wav  # explicit WAV path
run a:\MD.AXE a:\game.bin net         # TCP stream → Windows (see below)
```

Realtime audio over the QEMU OpenEth NIC (not UART):

```
argon run -Sd -Gfx -HostFs build\md_share
# guest waits after: run a:\MD.AXE a:\game.bin net
python tools/pcmplay.py                 # or: python tools/pcmplay.py --ffplay
```

`argon run` enables `hostfwd` on `127.0.0.1:5558` by default. Use `net:PORT` on the guest and `-NetPort PORT` on the host if you change it.

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

YM2612 timbre is approximate (six one-op voices + DAC in `ag_fm`). PSG uses the
same integer core as SMS. Native `ym2612.c` remains a measured fallback.
