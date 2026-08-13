# Doom (`DOOM.AXE`)

[doomgeneric](https://github.com/ozkl/doomgeneric) (Chocolate Doom) as a
guest `.AXE`. The engine draws 320×200 pal8; the port integer-scales nearest
(2× on the default 640×400 soft framebuffer) into RGB565.

Sound: SFX mix to `/dev/pcmvirt` (music mute). Needs `PCMVIRT.SYS`.
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
- `i_sound.c` — register Argon `DG_sound_module` without SDL; 256 KiB sfx cache
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
argon run -Gfx -Sd
```

Guest (once): `dir a:\kbdvirt.sys` then `drv install a:\kbdvirt.sys`
and `drv install a:\pcmvirt.sys` and `drv install a:\mousevirt.sys`
(`T:` is the RAM disk; the `.SYS` files are on `A:`).

```
run a:\doom.axe -iwad a:\doom1.wad pcmvirt
```

Host (three extra terminals):

```
python tools/kbdvirt.py --reconnect
python tools/mousevirt.py --reconnect
python tools/pcmplay.py --reconnect
```

Arrows move, Ctrl fire, Space use, Shift run, Enter, Esc menu.
Mouse: X turn, Y walk, LMB fire, RMB strafe, wheel weapons.
`kbdvirt.py` starts **paused**; Right-Ctrl toggles capture. Do not arm until
the title screen is up, or keys typed during WAD load get injected as a burst.
`mousevirt.py` also uses Right-Ctrl to pause (clicks then reach QEMU).

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
`nosound` / `pcmnull` mute SFX; default is `pcmvirt` (`pcmplay.py` on :5558).
QEMU SFX will stutter (seconds per frame); that is the renderer, not the mixer.
