# Doom (`DOOM.AXE`)

[doomgeneric](https://github.com/ozkl/doomgeneric) (Chocolate Doom) as a
guest `.AXE`. The engine draws 320×200 pal8; the port integer-scales nearest
(2× on the default 640×400 soft framebuffer) into RGB565.

Sound: SFX + music mix to `/dev/pcmvirt`. Music is MUS→MIDI through the
existing `ag_fm` synth (OPLL-style, not AdLib/Nuked). Needs `PCMVIRT.SYS`.
Mouse: `/dev/mouse0` via `MOUSEVIRT.SYS` (`mousevirt.py :5560`). Vanilla:
X turns, Y walks; LMB fire, RMB strafe, wheel prev/next weapon.

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
- `w_file_stdc.c` — 256 KiB WAD read cache (HostFS UART kills uncached R_Init)
- `r_data.c` — louder R_Init progress
- `doomtype.h` — DOS path separators (`\` / `;`) so `h:\doom1.wad` parses
- `i_sound.c` — register Argon `DG_sound_module` / `DG_music_module` without SDL; 256 KiB sfx cache
- `i_input.c` — `ev_mouse` from Argon `doom_argon_get_mouse`
- `i_video.c` — `usemouse = 1` under `ARGON_TARGET`
- `m_controls.c` — wheel → prev/next weapon under `ARGON_TARGET`

## Build

Needs `xtensa-esp32s3-elf-gcc` on PATH (IDF export).

```
python apps/doom/build.py
```

Builds `DOOM.AXE`, `KBDVIRT.SYS`, `PCMVIRT.SYS`, and `MOUSEVIRT.SYS`, stages
them into `build/sd_card/`, then bakes `build/sdcard.img` (put `doom1.wad` in
`build/sd_card/` yourself).

## Run

Input is **KBDVIRT**, not HostFS PADPUSH. WAD and `.AXE` come from the SD
image (`A:`).

```
argon run -Gfx -Sd -Virt
```

`-Virt` starts `pcmplay` + `kbdvirt` + `mousevirt` (`--reconnect`) in the
background and kills them when QEMU exits. Same helpers without QEMU:
`argon virt` (or `python tools/virt.py`).

Guest (once; `.SYS` files are on `A:`, not the RAM disk `T:`). Cwd is `A:\`,
so `virt` runs `VIRT.BAT`:

```
virt
```

Same as `call a:\virt.bat` — three `drv install` lines, then autoload on boot.

```
run a:\doom.axe -iwad a:\doom1.wad pcmvirt
```

Click the **QEMU RGB window** so `kbdvirt` captures keys (the serial console
keeps A:\\> typing). Right-Ctrl force-pauses kbd and mouse.

**WASD** walk/strafe, **mouse** turns, **LMB** fire, Space use, Shift run,
Esc menu, wheel weapons. Arrows still work in the menu.

The QEMU RGB window stays **black until the first presented frame** — after
`R_Init` / `P_Init` / `I_InitGraphics`. Watch `textures` / `flats` / `sprites`
dots. QEMU is slow on the software renderer (seconds to minutes per frame);
see `docs/07-emulator-performance.md`.

Warp straight into shareware E1M1:

```
run a:\doom.axe -iwad a:\doom1.wad -warp 1 1 -skill 3 frames90
```

`frames90` exits after 90 ticks (not a bare number — that would clash with `-warp`).
`livepad` turns HostFS PADPUSH back on if you really want it.
`nosound` / `pcmnull` mute SFX and music; default is `pcmvirt` (`pcmplay.py` on :5558).
QEMU audio will stutter (seconds per frame); that is the renderer, not the mixer.
