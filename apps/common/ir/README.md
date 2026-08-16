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
| Wet/gain | 0..127; gain 64 ≈ unity, wet lands 6–12 dB under dry |

Spectra live in PSRAM (`ag_malloc`). Hot path is one forward FFT, `parts`
complex MAC passes, one inverse FFT per block — ~44 parts for a full 500 ms
IR @ 22 kHz.

## Scaling — the part that was wrong

An IR is **normalised by its energy**, `sqrt(Σh²)`, not by its peak sample.
That is the gain a convolution actually applies: a 500 ms noise tail carries
hundreds of times the energy of a 20 ms cabinet with the same peak, and
peak-normalising both drove the output into the rail on every sample, at
which point the wet signal stopped depending on the input level at all.

The stored spectra pick their own shift (`ir->h_shift`) from the IR that was
loaded, and the output shifts back by the same amount. A fixed shift cannot
serve both ends of the range — it overflows the bins of one IR and leaves the
other's near zero, turning the convolution into rounding noise.

Preset IRs are reseeded on every load, so the same preset is the same room.

Linearity and the absence of full-scale steps are checked in
[`host-tests/test_dsp.c`](../../../host-tests/test_dsp.c).

Used by [`apps/irfx`](../../irfx).
