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
| FFT | 512-point int32, Q30 twiddles, real-input pair |
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

Those figures predate the tail and the twiddles, and neither moved the numbers
enough to remeasure the table. Counted from the ESP32-S3 disassembly: every
function in `ag_fft.c` compiles to exactly the instruction count it did with
Q15 twiddles — the multiply was already 32×32 into an `int64`, so only the
table grew — and `mac_part`, which is the whole of the per-partition cost, is
also unchanged. The output loop went from 52 instructions per sample to 63,
which is 1% of the fixed part and 0.3% of a 500 ms IR. It would have been 104
had the tail been carried in `int64`; taking only as many tail bits as the
block's scale already had room for keeps every shift a right shift and the
whole loop in `int32`.
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
| Overlap-add tail | rounded to the output word | carried `AG_IR_TAIL_BITS` below it |

Lifting the input into the room int32 had spare moves the transform's own
rounding down by as much as the lift, and that is what the first five rows are
for. The tail is the sixth and it is a different kind of mistake: the second
half of every convolution is held over and added to the block after it, and
rounding it to the output word on the way out and again on the way back in put
half a bit of white noise on every sample of it. Flat across the spectrum, so
loudest exactly where a loudspeaker is quietest. The tail was already `int32`
and using fifteen of its bits.

How far it can be lifted is not a guess. Every value anywhere in a radix-2
transform is a sum of its inputs with unit coefficients, so nothing in it can
exceed the sum of their magnitudes — an exact bound, computed per block in a
single pass. The first version used `max|x| * 512`, the worst case one bin can
reach, and paid three bits on audio and ten on an impulse response for a case
that does not occur.

## The twiddles

The largest single thing between this engine and the convolution it claims to
be was the twiddle table, and it was invisible for a long time because it is
the one error that does not behave like the others. A Q15 twiddle is wrong by
up to half a part in 32768, and that error is **multiplicative**: it scales
whatever passes through the rotation. Lifting the data words does not move it,
widening the accumulator does not move it, and it never shows up as a wrong
level, a nonlinearity or a failure to be repeatable — so every test in the tree
passed while it sat there.

Q30 costs nothing. The multiply was already 32×32 into an `int64`, so a wider
coefficient is the same instructions and 258 more bytes of table. Measured on
the transform alone, against an answer known exactly — the transform of one
sample at offset one is a pure rotation, so every bin must have the same
magnitude — the spread across bins went from 70 units in 2^20 to one.
[`host-tests/test_dsp.c`](../../../host-tests/test_dsp.c) asserts it, because
nothing else would notice it coming back.

## What it measures

Against the same convolution in double precision, on a 30 s guitar DI
(`build-host/ir_check <wav> <outdir> [ir.wav]`), through the synthetic 20 ms
cabinet:

| Input | Error before | Error now |
|---|---:|---:|
| 0 dB | −92.8 dBFS | −100.8 dBFS |
| −20 dB | −97.0 dBFS | −110.9 dBFS |
| −40 dB | −98.8 dBFS | −116.9 dBFS |

Worst single sample over the 30 s went from 10 LSB to 4.

Through a measured 500 ms Marshall cabinet (`build/nam/imp_mars.wav`, 44
partitions), which is the harder case and the one worth quoting, per band:

| Band | Error/signal before | Error/signal now | Band level |
|---|---:|---:|---:|
| 0.1–0.5 kHz | −67.5 dB | −77.9 dB | −16.7 dBFS |
| 0.5–2 kHz | −54.8 dB | −65.1 dB | −39.8 dBFS |
| 2–4 kHz | −42.1 dB | −49.0 dB | −57.8 dBFS |
| 4–6 kHz | −26.2 dB | −33.9 dB | −74.0 dBFS |
| 6–10 kHz | −20.7 dB | −26.6 dB | −78.4 dBFS |

## Where it stops, and why it stops there

Not at the output word. `ir_check` adds a known amount of noise to an *exact*
convolution before forming the output word, which says what accuracy the engine
is being asked for rather than what it delivers: 1 LSB of internal noise reads
as −21.9 dB in the 4–6 kHz band, 1/64 LSB reads as −41.4 dB, and none at all
reads as exactly zero. Six decibels per bit, all the way down. The output word
is not a floor, because the reference rounds to it too.

So the top-octave figures above are a statement about internal accuracy and
nothing else. −34 dB in the 4–6 kHz band is about 1/12 LSB; −60 dB there would
be about 1/570 LSB, nine bits below the word the result is written to. Reaching
it means every stage carrying those nine bits, and the measurements say the
stages have to move together — `H` exact on its own is worth 1.4 dB there, `X`
exact 0.2 dB, and `H`, `X` and the accumulator all exact 12.8 dB. Whichever
stage is coarsest sets the answer.

Two of them are cheap and are done: the twiddles above, and the overlap tail.
The rest is not. `X` and `H` in int32 is twice the PSRAM (348 KB → 696 KB for a
1 s IR at 22 kHz) and twice the memory traffic in the innermost loop, which is
already the engine's whole cost for a long IR. And it would land at about
−43 dB, not −60, because the next wall behind it is the spectrum handed to the
inverse transform: widening the accumulator past that point is provably free of
effect, since `ygain` renormalises the spectrum to the inverse's 2^29 sum bound
immediately afterwards and throws the extra bits away. Going further than −43
means the inverse in wider arithmetic as well, or a deliberate noise shaping
that spends the low end's 78 dB of margin on the top octaves.

Linearity and the absence of full-scale steps are checked in
[`host-tests/test_dsp.c`](../../../host-tests/test_dsp.c); the numbers above
come from `ir_check`, which is a tool rather than a test because it needs a
recording.

Used by [`apps/irfx`](../../irfx).
