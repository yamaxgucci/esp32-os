# ag_ir — partitioned IR convolution

Uniform **overlap-add FFT** convolution for ArgonOS apps (no libm).
Aimed at **≤500 ms** mono impulse responses @ ~22 kHz on ESP32-S3.

## API

```c
#include "ag_ir.h"

ag_ir_t ir;
ag_ir_init(&ir, 22050);
ag_ir_load_preset(&ir, 1);          /* 0=room 1=hall 2=spring */
/* or ag_ir_load(&ir, mono, frames, src_rate); */
ag_ir_process_block(&ir, mono256, stereo512);
ag_ir_free(&ir);
```

| Knob | Notes |
|------|--------|
| Block | `AG_IR_BLOCK` = 256 samples (~11.6 ms @ 22.05 kHz) |
| FFT | 512-point int32, Q15 twiddles |
| Cap | `AG_IR_MAX_MS` = 500 — longer WAVs are truncated |
| Wet/gain | 0..127; gain 64 ≈ unity after peak-normalize |

Spectra live in PSRAM (`ag_malloc`). Hot path is one forward FFT, `parts`
complex MAC passes, one inverse FFT per block — ~44 parts for a full 500 ms
IR @ 22 kHz.

Used by [`apps/irfx`](../../irfx).
