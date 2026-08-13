# Doom (`DOOM.AXE`)

[doomgeneric](https://github.com/ozkl/doomgeneric) (Chocolate Doom) as a
guest `.AXE`. The engine draws 320×200 pal8; the port integer-scales nearest
(2× on the default 640×400 soft framebuffer) into RGB565.

Sound is mute in this milestone.

## Licensing

| Component | Licence | Notes |
|---|---|---|
| `core/` | GPLv2+ | doomgeneric / Chocolate Doom / id Software |
| `doom_main.c`, `port/`, `doom_cfg.h` | Apache-2.0 | ours |

The shipped `.AXE` is a GPL work. **Do not put `doom1.wad` in this tree** —
supply the shareware IWAD yourself (freely redistributable).

## Local changes to the vendored core

- `doom_cfg.h` is `-include`d: `ARGON_TARGET`, `DOOMGENERIC_RESX/Y` 320×200
- `i_video.c` — `colors[]` is not static; `I_FinishUpdate` only calls `DG_DrawFrame`
- `i_video.h` — exports `colors[]` under `ARGON_TARGET`
- `doomgeneric.c` — dummy `DG_ScreenBuffer` (present path uses `I_VideoBuffer`)
- `i_system.c` — no zenity/`system()` GUI on `I_Error`; zone 4 MiB (heap is 5)
- `d_main.c` — skip melt wipe (each wipe step was a full-screen present)
- `doomtype.h` — DOS path separators (`\` / `;`) so `h:\doom1.wad` parses

## Build

Needs `xtensa-esp32s3-elf-gcc` on PATH (IDF export).

```
python apps/doom/build.py
```

Stages `build/apps/DOOM.AXE` and `build/sd_card/DOOM.AXE`.

## Run

Put shareware `doom1.wad` on the HostFS share (same folder as the `.AXE`).

```
argon run -Gfx -HostFs build\sd_card
run h:\doom.axe -iwad h:\doom1.wad
```

Warp straight into shareware E1M1 (handy for a timed QEMU test):

```
run h:\doom.axe -iwad h:\doom1.wad -warp 1 1 -skill 3 frames90
```

`frames90` exits after 90 ticks (not a bare number — that would clash with `-warp`).
QEMU is slow on the software renderer (seconds to minutes per frame);
that is expected, see `docs/07-emulator-performance.md`.

Pad (HostFS PADPUSH, same as MD/SMS): D-pad, B1 fire, B2 use, C run, Start
Enter, Pause Esc, Y/X = y/n, Quit exits. `nolivepad` falls back to serial
sticky keys (no key-up).
