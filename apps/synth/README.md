# SYNTH — VA / FM demo

Engine: [`apps/common/synth`](../common/synth). Output: [`ag_pcm`](../common/pcm).
Optional master [`ag_fx`](../common/fx) and cabinet [`ag_ir`](../common/ir) (presets 3/4).

## Build

```
python tools/mkaxe.py --arch xtensa --gcc xtensa-esp32s3-elf-gcc `
  --include sdk/include --include apps/common --include apps/common/dsp `
  --include apps/common/pcm --include apps/common/synth --include apps/common/fx `
  --include apps/common/ir --include apps/common/libc `
  --cflags "-Os -ffunction-sections -fdata-sections -fno-builtin" `
  -o build/apps/SYNTH.AXE `
  apps/synth/synth.c `
  apps/common/dsp/ag_dsp.c apps/common/pcm/ag_pcm.c `
  apps/common/synth/ag_osc.c apps/common/synth/ag_filt.c `
  apps/common/synth/ag_dist.c apps/common/synth/ag_fmx.c `
  apps/common/synth/ag_synth.c `
  apps/common/fx/ag_fx.c apps/common/ir/ag_ir.c apps/common/ir/ag_fft.c `
  apps/common/libc/libc_shim.c
```

## Run

`run h:\synth.axe pcmvirt` after `drv install h:\pcmvirt.sys`.
Host: `tools\pcmplay.py --reconnect` or `--record build\pcm_capture.wav`.

Keys: Z–M / Q–I notes, `[ ]` cutoff, `- =` reso, `1–6` wave (`6` = wavetable), `D` drive,
`T` tube/JFET, `F` VA/FM, `O` op count, `L` LFO→cutoff, `X` FX, `I` cab.
