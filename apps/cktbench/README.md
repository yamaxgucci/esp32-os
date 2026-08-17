# CKTBENCH — what an analogue circuit costs per audio sample

Answers "how many simulated components will the board carry in real time" in
the units the question actually has: instructions retired per sample, against
the 5000 cycles one 240 MHz core has at 48 kHz.

Analysis and conclusions: [`docs/08-circuit-simulation.md`](../../docs/08-circuit-simulation.md).

## Run it

`-Icount` is not optional. Without it the guest's clocks measure the host, not
the chip — see `check` below.

```
argon apps --only CKTBENCH.AXE
argon test -Icount -HostFs build\sd_card -TimeoutSec 2400 ^
    "run h:\cktbench.axe check h:\ckt_check.txt" ^
    "run h:\cktbench.axe prim  h:\ckt_prim.txt" ^
    "run h:\cktbench.axe parts h:\ckt_parts.txt 6000 48" ^
    "run h:\cktbench.axe baked h:\ckt_baked.txt 0 480 4 0" ^
    "run h:\cktbench.axe ramp  h:\ckt_ramp.txt 6000 48 4" ^
    "run h:\cktbench.axe mono  h:\ckt_mono.txt 6000 48 4"
```

Arguments after the mode are `outfile settle meas [stages] [flags]`. The report
goes to the file as well as the screen because the console transcript is a
rendered screen and anything that scrolls off it is gone.

`baked` takes flags: **bit 0** turns on antialiasing, **bit 1** folds a tone
stack into every stage. So `... 4 0` is four plain stages, `... 4 1` four with
ADAA, `... 4 3` both.

The firmware is only needed to run the guest; if `argon test`'s build step is in
the way, `tools\qemu-boot.ps1 -Icount -HostFs build\sd_card -Send @("...")`
does the same thing against whatever `build\argonos.bin` is already there.

| Mode | What |
|---|---|
| `check` | Is the clock telling the truth? Scaling with work, and the implied CPU frequency for three different workloads. |
| `prim` | Cost of the primitives: float divide, `exp`, `log`, `pow`, a table lookup. |
| `parts` | One sample through one circuit: RC, tone stack, diode clipper, JFET stage, 12AX7 clean and driven. |
| `baked` | 1..N valve stages with the nonlinearity solved in advance — the one that fits. |
| `ramp` | The same stages through the full solver, one matrix per stage. |
| `mono` | The same cascade as a single matrix, for the contrast. |

The column that answers the question is **ms/20ms**: how much processor time
20 ms of audio costs. Under 20 fits.

| | ms per 20 ms of audio |
|---|---:|
| Solver, one driven valve | 66.2 |
| Baked, four valves | 5.8 |
| Baked, four valves, ADAA | 9.6 |
| Baked, four valves, ADAA, twice oversampled | 19.2 |
| Baked, three valves, ADAA, twice oversampled | 15.0 |
| Baked, three valves + a tone stack, ADAA, twice oversampled | 17.4 |

`-Icount` slows the emulator by roughly an order of magnitude, so `parts` is
minutes and `ramp` is tens of minutes. The numbers are deterministic in
exchange: a repeat gives the same figure to the unit, on a loaded machine or an
idle one.

## Why the clock needs checking first

The first version of this program reported that `a = a*3 + 7` costs 0.109 CPU
cycles per iteration. Two defects, both in the measuring:

* `api->time->cycles()` returned microseconds — the same number as `us()` —
  which made it useless for the one job it exists for. Fixed to
  `esp_cpu_get_cycle_count()`.
* Under QEMU that counter is the virtual clock scaled by a 40 MHz core, and the
  virtual clock follows the host. `check` shows this directly: the implied
  frequency comes out 39–40 MHz for integer work, float work and `exp` alike.

With `-icount shift=0` one retired instruction advances virtual time by exactly
one nanosecond, so `ag_micros() * 1000` is an instruction count. `check`
confirms it: an integer chain measures **6.000** instructions per iteration at
4000, 40 000 and 400 000 repeats; a float chain, **9.000**.

Instructions are a floor, not cycles. A dependent FPU chain stalls on latency,
the cache misses, PSRAM answers late — QEMU models none of it. On silicon these
numbers can only get larger.

## The circuits

In [`ckt_circuits.c`](ckt_circuits.c), shared with the host tests so that what
is measured is also what is checked.

Apache-2.0.
