# GFXFM — graphical file manager

Same two-panel Norton-style manager as `apps/fm` / shell `fm`, drawn on the
soft RGB565 framebuffer (`ag_gfx_*`) instead of the text console.

## Build

Normally just `argon apps` (or `argon apps --only GFXFM.AXE`) — the
authoritative build line for this image lives in [`tools/apps.json`](../../tools/apps.json)
and is compiled by CI. The command below is the same thing, spelled out:

```bat
python tools/mkaxe.py --arch xtensa --gcc xtensa-esp32s3-elf-gcc ^
    --include sdk/include --include apps/fm ^
    --cflags "-Os -ffunction-sections -fdata-sections -DFM_GFX_BUILD" ^
    -o build/apps/GFXFM.AXE ^
    apps/gfxfm/gfxfm_app.c apps/gfxfm/fm_ui_gfx.c ^
    apps/fm/fm.c apps/fm/fmops.c
```

`mkaxe` also stages into `build/sd_card/`.

## Run (QEMU)

```bat
argon run -Gfx -HostFs build\sd_card
```

Then: `run gfxfm.axe` or `run gfxfm.axe t: c:`

Keys match text `fm` (F1 help, F3 view, F5 copy, F6 move, F7 mkdir, F8 delete,
Alt+F1/F2 drives, F10/Esc quit).
