# IRFX — impulse-response / convolution FX

Console app: partitioned FFT convolution, **≤500 ms** mono IR @ 22.05 kHz.
Built-in room/hall/spring presets, optional IR + dry WAV from HostFS.

Engine: [`apps/common/ir`](../common/ir). WAV: [`apps/common/wav`](../common/wav).

## Build

```
python tools/mkaxe.py --arch xtensa --gcc xtensa-esp32s3-elf-gcc `
  --include sdk/include --include apps/common --include apps/common/ir `
  --include apps/common/wav --include apps/common/libc `
  --cflags "-Os -ffunction-sections -fdata-sections -fno-builtin" `
  -o build/apps/IRFX.AXE `
  apps/irfx/irfx.c apps/common/ir/ag_ir.c apps/common/ir/ag_fft.c `
  apps/common/wav/ag_wav.c apps/common/libc/libc_shim.c
```

Stages to `build/apps/` and `build/sd_card/`.

## Run (QEMU)

```powershell
.\argon.cmd run -Gfx -HostFs build\sd_card
# guest:
#   drv install h:\pcmvirt.sys
#   run h:\irfx.axe pcmvirt
# optional: run h:\irfx.axe pcmvirt h:\irfx\room.wav h:\grain\demo.wav
```

Host: `tools\pcmplay.py --reconnect`.

## Controls

| Key | Action |
|-----|--------|
| Space | Trigger click / noise burst |
| A | Toggle auto trigger (~1.5 s) |
| B | Bypass |
| 1 / 2 / 3 | Preset room / hall / spring |
| S / N / W | Source: click / noise / dry WAV |
| `[` `]` | Wet − / + |
| `-` `=` | Gain − / + |
| Esc | Quit |

Live resource lines (refresh ~250 ms): **CPU** render/budget/load%, send,
loop, late; **MEM** arena used/free; **DSP** IR spectra + dry WAV KB; **PCM**
drop/ring when the sink supports GETSTATS.

## Args

`pcmvirt` / `pcmnull` / `pcmmix` / `audio`, optional `room`|`hall`|`spring`,
optional `ir.wav` then `dry.wav`.
