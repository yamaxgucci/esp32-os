# GRAIN — graphical granular synthesizer

Fullscreen soft-gfx instrument: waveform + live grain dots, Clouds-shaped
knobs (Pos / Size / Dens / Spray / Pitch / Tex), ADSR + lite FX, piano strip.
Loads 16-bit PCM WAV from HostFS. Mic **Rec** is a stub until capture hardware
exists; **Freeze** locks the buffer (append API ready for future mic).

Engine: [`apps/common/grain`](../common/grain). WAV: [`apps/common/wav`](../common/wav).

## Build

```
python tools/mkaxe.py --arch xtensa --gcc xtensa-esp32s3-elf-gcc `
  --include sdk/include --include apps/common --include apps/common/dsp `
  --include apps/common/pcm --include apps/common/grain `
  --include apps/common/wav --include apps/common/fx --include apps/common/libc `
  --cflags "-Os -ffunction-sections -fdata-sections -fno-builtin" `
  -o build/apps/GRAIN.AXE `
  apps/grain/grain.c apps/common/dsp/ag_dsp.c apps/common/pcm/ag_pcm.c `
  apps/common/grain/ag_grain.c apps/common/wav/ag_wav.c `
  apps/common/fx/ag_fx.c apps/common/libc/libc_shim.c
```

Stages to `build/apps/` and `build/sd_card/`. Demo sample:
`build/sd_card/grain/demo.wav` (also under `apps/grain/samples/`).

## Run (QEMU)

```powershell
.\argon.cmd run -Gfx -HostFs build\sd_card
# guest:
#   drv install h:\pcmvirt.sys
#   drv install h:\midivirt.sys
#   drv install h:\mousevirt.sys
#   run h:\grain.axe pcmvirt
```

Host (repo root), three helpers:

```powershell
$py = "D:\Espressif\tools\python_env\idf5.5_py3.12_env\Scripts\python.exe"
& $py tools\pcmplay.py --reconnect
& $py tools\midikbd.py --reconnect
& $py tools\mousevirt.py --reconnect
```

## Controls

| Input | Action |
|-------|--------|
| Mouse | Drag waveform → position; drag knobs; Load / Freeze / Rec; piano |
| Z..M / Q..I | Notes (same map as DX7 / midikbd) |
| L | Load WAV picker (`H:\grain`, `H:\`) |
| F | Freeze toggle |
| Arrows | Position / density |
| `[` `]` | Grain size |
| `-` `=` | Pitch |
| `;` `'` | FX wet |
| Space | All notes off |
| Esc | Quit |

## Args

`pcmvirt` / `pcmnull` / `audio`, optional `path.wav`, `nomidi`.
