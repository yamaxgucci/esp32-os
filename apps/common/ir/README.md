# ag_ir — partitioned IR convolution

Uniform **overlap-add FFT** convolution for ArgonOS apps (no libm).
Aimed at **≤1 s** mono impulse responses @ ~22 kHz on ESP32-S3.

## API

```c
#include "ag_ir.h"

ag_ir_t ir;
ag_ir_init(&ir, 22050);
ag_ir_load_preset(&ir, 1);          /* 0=room 1=hall 2=spring;
                                       3 and 4 are not cabinets, see below */
/* or ag_ir_load(&ir, mono, frames, src_rate); */
ag_ir_process_block(&ir, mono256, stereo512);
ag_ir_free(&ir);
```

| Knob | Notes |
|------|--------|
| Block | `AG_IR_BLOCK` = 256 samples (~11.6 ms @ 22.05 kHz) |
| FFT | 512-point int32, Q15 twiddles, real-input pair |
| Cap | `AG_IR_MAX_MS` = 1000 — longer WAVs are truncated. `AG_IR_PRESET_MS` = 500 fixes the synthetic reverbs regardless |
| Wet | 0..`AG_IR_WET_MAX` (128). **Each load sets it**: `AG_IR_WET_REVERB` (100) for presets 0–2, `AG_IR_WET_MAX` for the cabinet presets and for anything `ag_ir_load` builds. Call `ag_ir_set_wet` *after* the load for a mix |
| Gain | 0..127; 64 ≈ unity after the IR is normalised |

Spectra live in PSRAM (`ag_malloc`). Hot path is one forward FFT, `parts`
complex MAC passes, one inverse FFT per block — ~44 parts for a full 500 ms
IR @ 22 kHz.

Both transforms are the real-input pair, `ag_fft_real_fwd` / `ag_fft_real_inv`:
the signal going in is real and the signal coming out is real, so a transform
of half the length does almost all of the work. They are written to the same
convention and the same scale as the complex pair, which is why none of the
shift bookkeeping below knows the difference — the one exception is the bound
on the spectrum handed to the inverse, which is 2^29 rather than 2^30 because
the real inverse combines each bin with its mirror before it starts.

## Cost

Measured on the guest under `-icount` (`cktbench fx`), instructions per audio
sample. The IR must be warmed up for `parts` blocks before the stopwatch — an
unwritten slot of `X` is skipped, so a cold engine does almost no work.

| IR length | Partitions | KB | Instr./sample | % of a core @22.05 kHz |
|---|---:|---:|---:|---:|
| 20 ms | 2 | 8 | 981 | 9.0% |
| 100 ms | 9 | 36 | 1349 | 12.3% |
| 250 ms | 22 | 88 | 2030 | 18.6% |
| 500 ms | 44 | 176 | 3179 | 29.2% |
| 1000 ms | 87 | 348 | 5424 | 49.8% |

That is 981 for the fixed part — of which 678 is the transform pair, against
990 for the complex pair it replaced — plus **52 per partition**. The limit on
IR length is the processor, not the memory: 348 KB sits in PSRAM unnoticed.
What `-icount` does not show is that those 348 KB are read every block, 30 MB/s
out of external memory; only the board can say what that costs.

The whole story — five changes, 2453 down to 981, with no loss of accuracy —
is in [docs/08-circuit-simulation.md](../../../docs/08-circuit-simulation.md).

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

The wet control goes to **128**, not 127, because the mix divides by 128.
Stopping a notch short leaves 1/128 of the dry signal — 42 dB down, which is
nothing behind a reverb and wrong behind a cabinet, whose whole job is to
remove a top end that the leak then puts back. It was not, as first supposed,
loud enough to be the fizz someone was hearing: measured, it sits 30 dB under
the convolution's own top end. It is still not something a cabinet should
have.

## The dry leak, which was loud enough

The 127 above is the harmless version of a mistake the default made in
earnest. `ag_ir_init` left `wet` at 100, and every caller that loaded a
cabinet and did not then call `ag_ir_set_wet` ran with **22% of the dry signal
— about −13 dB — around the convolution**.

Behind a reverb that is inaudible, which is why the number survived. Behind a
cabinet it is not a leak, it is the output. A dry guitar signal still has its
whole top end; a loudspeaker is 17 dB down at 8 kHz. The leak is therefore
*louder than what the cabinet passes* across the top octave, and it does not
subtract a little — it removes the roll-off. The same Vox AC30 impulse,
measured band by band, relative to 1250 Hz:

| | 2.5k | 4k | 5k | 6.3k | 8k | 10k |
|---|---:|---:|---:|---:|---:|---:|
| wet 128 | +6.3 | −0.0 | +2.3 | −3.1 | **−16.5** | **−14.6** |
| wet 100 | +8.1 | +6.7 | +8.2 | −8.6 | **−1.6** | **−5.0** |

Fifteen decibels at 8 kHz, from the one knob nobody set. What it looked like
from the outside was a render where dry and "cabinet" sat 1–2 dB apart in
every band, indistinguishable by ear, and an amplifier that appeared to hiss —
a modelling fault that was not one.

So the loads set `wet` themselves now, from what the impulse is: `ag_ir_load`
and presets 3–4 go to `AG_IR_WET_MAX`, presets 0–2 to `AG_IR_WET_REVERB`. Both
directions, so switching a cabinet back to a hall brings the hall's dry signal
back with it. A caller that wants a mix says so after the load, which is the
one place it can be said with the impulse already known.

## Presets 3 and 4 are not cabinets

They are one large first tap (24000) plus 20 ms of low-passed noise that
peaks near 1250 — 26 dB down. The tap dominates so completely that what comes
out is very nearly a bypass. Measured fully wet, relative to 1250 Hz:

| | 2.5k | 4k | 5k | 6.3k | 8k | 10k |
|---|---:|---:|---:|---:|---:|---:|
| preset 3 "cab dark" | −1.3 | −0.1 | +0.0 | −0.2 | +0.2 | +0.2 |
| preset 4 "cab bright" | +0.4 | −1.3 | +0.3 | −0.9 | −0.7 | −1.1 |
| a real cabinet | +6.3 | −0.0 | +2.3 | −3.1 | −16.5 | −14.6 |

Flat within a couple of decibels end to end, and **no roll-off whatever** above
2.5 kHz, where the whole point of a cabinet lives. They are useful for what
they were written for — a short IR to measure the convolution's cost and
scaling against — and useless as the thing you judge a distortion model
through, because judging one through a near-bypass is judging it through
nothing.

The real impulse is in the tree:
`assets/audio/guitar-di/1 Vox AC30 1.wav`, 7234 taps at 48 kHz (151 ms),
24-bit, +2 dB at 2 kHz and −17 dB at 8 kHz. `tools/tube_render.c` defaults to
it, and its `run_cab` prints whichever impulse it used together with that
impulse's own response, warning when the shape is not a loudspeaker's;
`tools/ckt_play.c` carries the same check. Both exist because a convolution
with the wrong impulse sounds exactly like a modelling problem.

```bash
build-host/tube_render.exe cab
```

## Where the bits go, and where they went

Every fixed shift in this path was audible as hiss, and none of them had to
be there. Four now follow the signal instead:

| | Was | Is |
|---|---|---|
| Input to the forward FFT | int16 as it arrives | lifted per block, `fft_headroom` |
| IR into `part_fft` | int16 as it arrives | lifted per IR, `ir->h_pre` |
| Input spectrum → int16 | fixed `>> 9` | per block, recorded in `ir->x_sh` |
| Partition product | `>> 15` | `ir->p_shift`, the least the partition count allows |
| Spectrum into the inverse FFT | as it fell out | lifted per block, given back in the output shift |

The transform is the one worth explaining. It truncates a Q15
product in every butterfly, so it injects about a unit of noise per stage and
the following stages amplify it — roughly 6 LSB at the output, whatever went
in. Against int16 taps whose rms is a few hundred that is 40 dB of noise on
everything the convolution ever produces, and it does not go down when the
music does. Lifting the input into the room int32 had spare moves it down by
as much as the lift.

How far it can be lifted is not a guess. Every value anywhere in a radix-2
transform is a sum of its inputs with unit coefficients, so nothing in it can
exceed the sum of their magnitudes — an exact bound, computed per block in a
single pass. The first version used `max|x| * 512`, the worst case one bin can
reach, and paid three bits on audio and ten on an impulse response for a case
that does not occur.

Measured against the same convolution in double precision, on a 30 s guitar
DI through a 20 ms cabinet (`tools/ir_check.c`):

| Input | Error before | Error now |
|---|---:|---:|
| 0 dB | −68.4 dBFS | −92.8 dBFS |
| −20 dB | −84.0 dBFS | −96.7 dBFS |
| −40 dB | −87.1 dBFS | −98.1 dBFS |

Worst single sample over the 30 s went from 194 LSB to 12.

## Where it stops, and why it stops there

At the int16 input spectrum. Cutting `X` by three bits costs 12 dB of
accuracy; lifting the transforms further, or widening anything else, costs
nothing — which is the measurement that says everything before it has stopped
mattering. What is left is about 5 dB above the output word's own floor, and
buying it back means `X` in int32: twice the memory (192 KB → 385 KB for a
500 ms tail) and an int32 × int16 multiply in the innermost loop. Not worth
five decibels under a signal 64 dB above them.

Linearity and the absence of full-scale steps are checked in
[`host-tests/test_dsp.c`](../../../host-tests/test_dsp.c); the numbers above
come from `build-host/ir_check <wav>`, which is a tool rather than a test
because it needs a recording.

Used by [`apps/irfx`](../../irfx).
