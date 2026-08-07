# SMS — Master System player for ArgonOS

Mute port of **SMS Plus GX** (GPLv2+) as an ArgonOS `.AXE`. Video via soft
`gfx`; sound disabled; keyboard maps to the pad.

## License

- `core/` and `LICENSE` — SMS Plus GX, **GPLv2 or later**
- `sms_main.c`, `port/` (except where noted) — Apache-2.0

The kernel stays Apache-2.0; this application image is a separate GPL work.

## Build

```
python tools/mkaxe.py --arch xtensa --gcc xtensa-esp32s3-elf-gcc `
  --include sdk/include --include apps/sms/port --include apps/sms/core `
  --include apps/sms/core/z80 --include apps/sms/core/sound `
  --cflags "-Os -ffunction-sections -fdata-sections -DLSB_FIRST -DNOZIP_SUPPORT -DUSE_Z80 -Wno-unused -Wno-sign-compare" `
  -o build/SMS.AXE `
  apps/sms/sms_main.c apps/sms/port/libc_shim.c apps/sms/port/platform.c `
  apps/sms/core/loadrom.c apps/sms/core/memz80.c apps/sms/core/pio.c `
  apps/sms/core/render.c apps/sms/core/sms.c apps/sms/core/system.c `
  apps/sms/core/tms.c apps/sms/core/vdp.c `
  apps/sms/core/z80/z80.c `
  apps/sms/core/sound/sound.c apps/sms/core/sound/fmintf.c `
  apps/sms/core/sound/ym2413.c apps/sms/core/sound/sn76489.c
```

`-DLSB_FIRST` is required (Xtensa is little-endian; the Z80 core's `PAIR` union
depends on it). Large carts keep ~1 MB BSS in PSRAM; code stays under the IRAM
arena today (R-1 XIP applies if `.text` grows past 64 KB).

## Run (QEMU)

```
argon test -Put build\SMS.AXE=t:\sms.axe -TimeoutSec 180 `
  "run t:\sms.axe" "errorlevel" "gfxdump t:\sms.ppm"
```

Optional args: `run t:\sms.axe t:\game.sms 300` (ROM + frame count), or
`run t:\sms.axe 60` (built-in tiny cart, N frames). Without a ROM, the built-in
cart still exercises gfx.

## Controls

| Key | Pad |
|-----|-----|
| Arrows / WASD | D-pad |
| Z / J | Button 1 |
| X / K | Button 2 |
| Enter | Start |
| Esc / Q | Quit |
