# GFXBENCH — Argon soft-gfx vs LVGL

Same player-like scene (panels, 5 buttons, seek/volume, 10 EQ sliders,
16 spectrum bars, 12 playlist rows) on two backends, same `ag_gfx_flush`
present path.

| Image | Backend |
|---|---|
| `GFXBENCH.AXE` | kernel `ag_gfx_*` (fill/blit/text) |
| `LVGLBENCH.AXE` | LVGL 9.3 widgets → direct RGB565 into the gfx back buffer |

LVGL is **not** in the kernel. The tree is fetched into `third_party/lvgl`
(gitignored) at build time.

## Build

Needs `xtensa-esp32s3-elf-gcc` on PATH (IDF export).

```
python apps/gfxbench/build.py
```

`--native-only` / `--lvgl-only` if you only want one image. `mkaxe` stages
into `build/apps/` and `build/sd_card/`.

Typical image size (xtensa `-Os`, LVGL 9.3 trimmed widgets):

| Image | code | data (stored) |
|---|---:|---:|
| `GFXBENCH.AXE` | ~5 KB | ~0.5 KB |
| `LVGLBENCH.AXE` | ~137 KB | ~18 KB (font + theme) |

LVGLBENCH is ~137 KB of code. The S3 firmware reserves a **192 KB** executable
arena (`CONFIG_ARGON_APP_ARENA_KB`); `SYSTEM.CFG` may clip it (docs example is
64 KB). Below the image size the loader uses flash XIP instead of IRAM.

## IRAM vs XIP (fair toolkit comparison)

1. In the guest: `mem` — look for `N KB reserved for application code`.
2. If it is already **192 KB** and a previous `run h:\lvglbench.axe` log had
   `code … B at 0x3…` (no `XIP`), that run was already IRAM. Stop here.
3. If it is **64 KB** (or the log said `XIP at 0x42…`):

```
copy h:\arena192.cfg c:\system.cfg
reboot
```

If `C:\SYSTEM.CFG` already has `[modules]` (pcmvirt etc.), do not overwrite:
open it in `edit` and add the `[memory]` block from `h:\arena192.cfg`.

4. After reboot, `mem` must show 192 KB. Then:

```
run h:\lvglbench.axe full 300
```

Success: loader line is `LVGLBENCH: … code 140228 B at …` **without** `XIP`.
Still XIP: another image is occupying the arena (`ps` / `drv`), or the running
firmware was built with a 64 KB ceiling (rebuild: `argon build`).

`copy` of `arena192.cfg` is staged next to the `.AXE` files in `build/sd_card/`.

## Run (QEMU)

```
argon run -Gfx -HostFs build\sd_card
```

```
run h:\gfxbench.axe full 300
run h:\lvglbench.axe full 300
run h:\gfxbench.axe dirty 300
run h:\lvglbench.axe dirty 300
run h:\gfxbench.axe idle 300
run h:\lvglbench.axe idle 300
```

Modes:

| Arg | What happens |
|---|---|
| `full` | animate seek, spectrum, EQ, volume, selection; redraw everything |
| `dirty` | animate seek + spectrum + time only; partial update |
| `idle` | static UI, still full redraw each frame (AMP-like) |

A number is a frame count (then exit). Esc/Q quits. Stats every 2 s:

```
gfxbench: 12 fps, work 80000 us (draw 12000, flush 68000), max 90000 us
gfxbench mem: arena …/… free, largest …, fast …, sys …
```

QEMU numbers are **relative** only — HostFS/soft present is not ESP32-S3
PSRAM latency. Compare the two images on the same run, same mode.

## What this does not measure

AMP’s MP3/EQ/HostFS load. That is a later `AMPLVGL` slice. This bench is
raster + widget overhead on the shared flush path.
