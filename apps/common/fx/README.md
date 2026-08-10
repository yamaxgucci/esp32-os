# ag_fx — lite master-bus FX

Fixed-point **delay → chorus → reverb** chain for ArgonOS apps (no libm).
Intended for ~22 kHz stereo s16 chunks on ESP32.

## API

```c
#include "ag_fx.h"

ag_fx_t fx;
ag_fx_init(&fx, 22050);
ag_fx_set_enable(&fx, AG_FX_DELAY | AG_FX_CHORUS | AG_FX_REVERB);
ag_fx_process(&fx, stereo_interleaved, frames);
ag_fx_free(&fx);
```

| Stage | Notes |
|-------|--------|
| Delay | Circular buffer ≤370 ms, feedback + mix; light L/R tap offset |
| Chorus | 2 modulated taps (triangle LFO), ~7–25 ms |
| Reverb | Schroeder / Freeverb-lite: 4 comb + 2 allpass per channel |

Buffers are `ag_malloc`'d in `ag_fx_init` (~25 KB @ 22 kHz). Enable bits and
per-stage mixes are independent; `master_wet` blends the chain vs dry.

Used by [`apps/dx7`](../../dx7) (`nofx` / `fx0` / `fx1`).
