# ag_tube — a valve stage as one static curve, with its network beside it

Two 12AX7 stages of a hot-rodded JCM800 front end. Each valve is a **static
transfer curve baked from the circuit solver**; everything around it that has a
frequency in it — coupling capacitors, cathode bypasses, the grid stopper — is
linear, so it lives in **biquads** instead. A Wiener–Hammerstein chain, not a
waveshaper, and not memoryless: the drive into each valve depends on frequency,
and the DC that an asymmetrically clipping stage pushes onto its coupling
capacitor is here too, with the right time constant.

```
in ─▶[drive]─▶ F1 ─▶ ╔═ ×4 ══════════════════════╗ ─▶ F3 ─▶[master]─▶ cabinet
                     ║  T1 ─▶ F2 ─▶ T2  (+ADAA)  ║
                     ╚═══════════════════════════╝
```

| File | What |
|---|---|
| `ag_tube.h` / `.c` | the curve: bake by DC sweep, lookup, antialiasing average |
| `ag_biq.h` / `.c` | first and second order sections, and the designers for them |
| `ag_os.h` / `.c` | halfband ×2 and ×4, up and down |
| `ag_amp.h` / `.c` | the chain, the JCM800 values, the axis fit |

Measured by [`host-tests/test_tube.c`](../../../host-tests/test_tube.c) and
[`tools/tube_render.c`](../../../tools/tube_render.c); cost on the chip by
[`apps/tubebench`](../../tubebench). Read next to
[`docs/08-circuit-simulation.md`](../../../docs/08-circuit-simulation.md), which
is the same amplifier built the other way.

## How this emulator is built

**This section is the specification. Everything else in this file describes how
far the code has got towards it, and where it has not.** It was set out by the
author of the project on 18 August 2026, after two models had been built the wrong
way round; those two are being rebuilt to it.

**1. Start from a real schematic.** Build the actual circuit. **Every knob at the
middle** unless something says otherwise. The gain of a model comes from its
schematic — plate loads, stage count, bias — and never from where somebody chose
to leave a pot.

**2. Three kinds of block, and they must not be confused with each other.**

| block | what belongs in it | at run time |
|---|---|---|
| **knob blocks** | every control, with the network around it: the tone stack, the gain pots | **exposed as live real-time controls** |
| **stages and their network** | the reactive parts around each gain or clipping stage — coupling capacitors, cathode bypass, grid stopper. Everything else on the DC path is **baked into that stage's table** | fixed by the bake |
| **tone-matching blocks** | **filters in front of the stages**, and **one IR after everything** | moved only by the fit |

**3. Tone matching is done with the matching blocks alone, with every knob at the
middle.** `cfg.mid_hz / mid_db / mid_q` and `cfg.voice[]` **are** those filters.
They are the point of the design and not residue to be tidied away: this file
previously proposed deleting `mid_*` and putting the real tone stack in its place,
and that was wrong twice over. The stack is a knob block. `mid_*` is a matching
block. Both exist, in different positions, for different reasons.

Why the separation is worth being strict about: a control has to be turnable while
a note is sounding and has to sit where the schematic puts it, while a matching
filter exists only to close the distance between this chain and a capture of the
real amplifier. Blur the two and every number becomes unarguable — a "component
value" that was actually dialled by ear, or a fitted band standing in for a network
that was never built. Both of those have already happened here, and each cost a
rebuild.

### What follows from it, and what it costs

- **A pot at the middle is 0.5, not the value that makes the model sound right.**
  `g12` was 0.55 in the crunch model and 1.0 in the three-valve one, both argued
  for rather than measured, and the second one was half of why that chain came out
  19 dB quieter than a two-valve Marshall. Under this rule the pot goes to the
  middle and the gain has to come out of the netlist.
- **`cfg.gain[i]` must not be a divider standing in for a circuit.** The 0.15 in
  front of the cold clipper is the insertion loss of a tone stack (measured on the
  real network at noon: −13.4 dB at 1 kHz) times a pot at the middle. Under the rule
  the stack itself goes in as a knob block and that constant disappears — not the
  matching filters, which stay.
- **After the stages there is exactly one place, and it is the IR.** The post-valve
  `cfg.tone[]` bank does not exist in this scheme; what it currently supplies —
  +8 to +16 dB of fitted presence and low end — has to move into a matched impulse
  response. That is not a cosmetic move: a bank in front of the valves provably
  cannot supply it (fitted there, both top bands ran into their ±18 dB limit and
  were still 5 dB short), so the IR is where it has to live.
- **Component values need provenance, and the three kinds are not equal.**
  `ag_tube_spec_jcm800` is the strongest: the same numbers live in
  `apps/cktbench/ckt_circuits.c` and `test_tube.c` checks them field by field.
  `ag_tube_spec_slo` is next: two independent public analyses of the SLO-100 that
  agree value for value. `ag_tube_spec_bogner` is a documented *topology* — a Shiva
  crunch channel is described as a 2203 with three gain stages, a cathode follower
  and a hotter second valve — with the 2203's own values under it and one departure
  ("much hotter") that is an interpretation and says so. Both new ones replaced a
  family resemblance, and what that was worth is measured two sections down.

### Where the three models stand against it

| | `jcm800` | `bogner` | `slo` |
|---|---|---|---|
| values from a schematic | **yes**, 2203, checked twice | **yes**, a Shiva crunch channel | **yes**, an SLO-100 overdrive |
| knobs at the middle | yes | **yes**, gain pot at 0.5 | **yes**, gain pot at 0.5 |
| tone stack as a knob block | **yes** — `ag_tone`, live pots at noon | **yes**, from a 1k follower | **yes**, values substituted |
| stage network in biquads | yes | yes | yes |
| rest baked into the table | yes | yes | yes |
| matching filters in front | yes | yes | yes |
| matching after the stages | a biquad bank, **should be the IR** | same | same |

So the parts this tree got right are the ones about the stages: a curve baked from
the solver, its reactive network in sections around it, one table per stage. What
was missing was the controls, and the first of them is now in: `ag_tone`, the
passive stack, in the JCM800 where the values are real.

### The first knob block, and what it was worth

`ag_tone.h` is the whole story of the piece; this is what it changed. The stack
goes where a 2203 has it — after V1b's coupling capacitor, before the master — with
its three pots at noon, and it is **live**: five keys in `tube_live` move
resistances in a passive network, which is solved again while the note carries on
through it.

Then the matching was refitted around it, and **it had less to do**:

| | without the stack | with it |
|---|---|---|
| what the pre bank asks for | +2.70/+2.54/−0.45/+5.97 dB | −0.21/+3.78/+0.43/+6.21 |
| what the post bank asks for | +6.12 bass, +9.95/+14.94 presence | **−1.09 bass, +7.45/+12.55** |
| third-octave rms vs the capture | 1.27 dB, worst 2.7 | **0.97 dB, worst 1.8** |
| 3.5–5 kHz humps (capture +8.8) | +10.8 dB | **+10.0 dB** |
| `master` for −1 dBFS | 1/1200 | 1/255 |

The bass boost the fit used to need was the network's own; six decibels of it went
away the moment the network was there to supply it, and the error fell with it.
That is the argument for the whole architecture in one row: **a matching filter
standing in for a missing circuit is doing two jobs and neither of them the way
the amplifier does.** The master moved because a passive Marshall stack at noon
throws away 13.4 dB at 1 kHz — an insertion loss, not a level choice.

What it costs, on the guest under `-icount`: **69 instructions a sample**, 0.67% of
a core, the same figure at ×1 and at ×4 — which is how a number proves it is
outside the oversampled region. The specialisation is exact rather than
approximate — same discretisation, same theta — and `test_tone_stack` proves it by
building the network both ways and running both. Through `ag_ckt` instead it would
be a six-node solve every sample *and* would drag the circuit solver into an
application that plays presets, which is what the preset format exists to avoid.

**And 69 is the second answer.** The first was 249, for the identical arithmetic
written as a four-by-four loop with two-dimensional indexing: fifteen instructions
per multiply-add, nearly all of it address arithmetic and loop counters. Written
out flat it is 69. The estimate before either measurement was "sixteen
multiply-adds, call it 45", which makes three times in this file that counting
arithmetic has been wrong about what the processor does with it.

**And the netlist that started this was wrong.** `ckt_build_tonestack` in
`apps/cktbench` stamped both halves of the treble pot between the same pair of
nodes: the wiper was not a node at all, so the pot was one 62.5 k resistor and the
output came off the bass node — a tone stack with no treble control in it. It cost
nothing there, because that netlist exists to be *timed* and nobody had looked at
its response; it cost something the day its response was measured and quoted here
as "the real network at noon, −9.9 dB at 1 kHz". The corrected network at noon is
**−13.4 dB at 1 kHz, +9.6 dB at 40 Hz, a decibel of dip at 630 Hz and +3.4 dB by
10 kHz** — the mid scoop everyone knows, which the broken one did not have. The
benchmark netlist is fixed; its `parts` row is now a six-node solve and a little
dearer than last measured.

## Does a static curve give intermodulation?

Yes, and that was never the question that separates this from a waveshaper. Any
memoryless nonlinearity expands `a·cos ω₁t + b·cos ω₂t` into terms at every
`mω₁ ± nω₂` from the cubic term onwards. That is algebra, not a property of a
valve; a waveshaper does it too.

What a static curve does **not** have is memory inside the nonlinearity. That is
implemented for the grid — see below — and still missing for supply sag and for
the cathode bypass being pumped by large-signal cathode current. All three reach
the valve as a shift of the same voltage, which is why `ag_tube_tick_bias` takes
one.

## Blocking distortion

The grid is a diode. Driven above the cathode it conducts, and that current has
to come through the input coupling capacitor, which charges; the only way back
is the grid leak, so the charge sits there and holds the grid down. The stage
biases *itself* toward cut-off, goes quieter and more asymmetric, and recovers
over milliseconds.

```
C dvc/dt = ig(v - vc) - vc/rgrid
```

and note what is **not** in that equation. The full node equation is
`C dvc/dt = (v - vc)/rgrid + ig`, whose linear part is the coupling capacitor's
high pass — and that is already a biquad in F1 and F2. It is linear in `vc` for
a given `ig`, so the two superpose exactly, and taking the whole thing here
models the same capacitor twice. Written that way the first version put a DC
step entirely across the capacitor, which is the high pass doing its job and has
nothing to do with blocking. What is left is the extra charge the grid puts
there.

Two more things the bake has to get right. `ig` comes from `ag_triode_eval` at
the same solved operating point as the curve beside it, so it is the same valve;
and it is **zeroed at the operating point and clamped at zero below**, because
Koren's soft knee returns something very small rather than nothing, and "very
small" is not zero once it has been integrated onto a capacitor for a hundred
milliseconds. Without that the amplifier is not silent when nothing is playing.

What it does, measured on a real take at drive 0.5 — everything else identical:

| | V1b's grid window | lookups on the flat tail | peak |
|---|---|---:|---:|
| blocking off | −18.2 … +25.9 V | 66 176 | 0.793 |
| **blocking on** | **−24.1 … +11.3 V** | 125 968 | 0.853 |

The window moves down fourteen volts: the stage has biased itself toward
cut-off. Against the same render without it, the difference is only 5.3 dB below
the signal overall and 1.6 dB below it in the 1.5–5 kHz band, so this is not a
subtlety.

**The two stages behave completely differently, and that is the design.** V1a's
100 nF into 1 M drains in 100 ms — the slow choke-and-bloom — but at drive 1.0
V1a barely reaches conduction at all, so its charge peaks at a few millivolts.
V1b's 2.2 nF into 470 k drains in **1 ms**, so its charge follows the waveform
cycle by cycle and works as a fast asymmetry rather than a slow envelope. That is
precisely why a high gain amplifier puts a small capacitor in front of the valve
that clips: it gets the bias shift without the note choking.

It costs 2.9 dB of non-harmonic products under 5 kHz (−70.3 → −67.4 at ×4 with
ADAA), which is the one thing worth watching: the antialiasing average assumes a
fixed piecewise-linear curve and blocking slides that curve sideways every
sample, so what it generates is covered by the oversampling and not by ADAA.

## What the bake produces

Baked by sweeping the same `ag_ckt` solver over DC, so there is exactly one Koren
model in the tree. `tube_render curve`:

| | V1a | V1b |
|---|---:|---:|
| plate load / cathode | 220 k / 820 Ω | 100 k / 820 Ω |
| quiescent plate | 131.57 V | 191.61 V |
| quiescent cathode | 0.740 V | 1.135 V |
| gain, cathode bypassed | −71.7 | −59.8 |
| gain, cathode open | −51.2 | −37.6 |

Three decisions inside that sweep, each of which changes the sound:

**The load line is the AC one.** The next stage hangs on the plate through a
capacitor, and that capacitor holds the quiescent plate voltage — so at audio
frequencies the load is `rload` from the plate **to `vq_plate`**, not to ground.
Worth about 15% of gain a stage, 30% over two.

**The cathode is held at its operating point** and the shelf that appears below
1/(2π·Rk·Ck) goes in front of the curve, not after it. Cathode resistance is
local feedback: it reduces gain *and* distortion. A shelf after the valve would
only take back the level. The depth is not dialled — it is the ratio of the two
small-signal gains the bake measured: **285 Hz, −2.93 dB for V1a and −4.02 dB
for V1b.**

**Grid current is in the curve.** Without it the positive half never limits, the
stage clips only into cut-off, and the asymmetry is the wrong way round.

## The axis is fitted to the signal, and that is not optional

The grid conducts through 78 k, so the curve never plateaus:

| source volts into V1a | plate volts left before saturation, of 266 |
|---:|---:|
| +1 | 63 |
| +8 | 39 |
| +32 | 16 |
| +128 | 4.5 |

An axis fitted to where the curve *finally* stops moving runs to two kilovolts:

| `axis_tol` | axis reaches |
|---:|---:|
| 0.08 | +27 V |
| 0.01 | +175 V |
| 0.002 | +780 V |
| 0.0005 | +2 kV |

while the knee that decides how the valve sounds is a volt and a half wide. So
five sixths of a curve-fitted table lies where the music never goes, and
measured on the same two seconds in the 1.5–5 kHz band, **the best curve-fitted
axis is 48 dB from a signal-fitted one, and no table size recovers it.**

`ag_amp_build` therefore fits the axes to a probe signal, three passes, unioning
the ranges rather than replacing them so that it converges instead of chasing
its own tail. Outside the axis the curve is extended flat, exactly — including
inside the antialiasing average, which accounts for the parts of a span that lie
beyond either end rather than clamping the endpoints and hoping.

Reading off the end is **not** the model guessing here, unlike the two-axis
table: below cut-off the curve really is flat. On a real take at drive 1.0 about
11% of lookups land on the cut-off plateau, which is the second valve being cut
off for part of the cycle — which is what a slammed 12AX7 does. The counter is
kept and printed anyway, because "flat enough" is a claim.

## How big the table has to be

`tube_render res`, antialiasing off so that this is interpolation and nothing
else, axes held still, against a reference 64 times finer:

| points | all | 1.5–5 kHz | memory for the pair |
|---:|---:|---:|---:|
| 256 | −60.1 dB | −52.9 dB | 4 KB |
| 512 | −70.2 dB | −64.1 dB | 8 KB |
| 1024 | −79.0 dB | −74.4 dB | 16 KB |
| **2048** | **−88.2 dB** | **−83.8 dB** | **32 KB** |
| 4096 | −97.0 dB | −90.3 dB | 64 KB |
| 8192 | −93.4 dB | −88.8 dB | 128 KB |

Every doubling is worth 6 to 11 dB, which is what a piecewise-linear
approximation of a smooth curve should give — and then 8192 comes back *worse*
than 4096, because at that point it is within a factor of sixteen of the
reference and the number has reached the reference's own floor rather than its
own.

For scale, the two-axis model reaches −57.7 dB in the same band and occupies
139 KB for two stages. **28 dB better on a quarter of the memory**, and the
reason is not cleverness: a triode's plate swings 300 V, so ten millivolts of
interpolation error is nothing, while the second axis the other model needs is
genuinely expensive.

## Oversampling, and why a table needs it at all

A lookup works with amplitudes and knows nothing about frequency — but a
sequence of samples is a frequency-domain object whether the code treats it as
one or not. A nonlinearity *creates* frequencies: at 22.05 kHz the twelfth
harmonic of a 1 kHz note is 12 kHz, cannot exist in the grid, and arrives at
10.05 kHz, fifty hertz from the real tenth harmonic, where the two beat. Every
sample of that is computed correctly; it is the signal they represent that is
wrong.

`tube_render alias`, a 1060.5 Hz tone at drive 1.0, bin 197 of 4096 so that
folded harmonics land 41 bins off the grid:

| | all bands | **under 5 kHz** | host ns/sample |
|---|---:|---:|---:|
| ×1 | −18.4 dB | −23.0 dB | 160 |
| ×1 + ADAA | −26.4 dB | −40.4 dB | 295 |
| ×2 | −29.5 dB | −33.5 dB | 345 |
| ×2 + ADAA | −38.2 dB | −56.0 dB | 800 |
| ×4 | −37.8 dB | −51.5 dB | 710 |
| **×4 + ADAA** | −38.2 dB | **−68.0 dB** | 1290 |

Read the second column. The whole-band figure has a floor that is not the
nonlinearity: the halfband decimator's transition runs from about 9.2 to
12.8 kHz, so the eleventh harmonic of this tone is only partly removed and folds
back just under Nyquist. Above 5 kHz a guitar cabinet is thirty decibels down,
so that floor is not what anyone hears — and it is what made ×4 look no better
than ×2 until the columns were split.

**ADAA is worth 17 dB at ×1 for 1.8× the cost**, which is the best trade in the
chain, and ×2 with it beats ×4 without.

## The antialiasing average, and the arithmetic that nearly went wrong

The average of the curve over the span the input crossed since the last sample.
Three paths, and which one runs is decided by the size of the step:

* **inside one cell** the curve is a straight line and its average is its
  midpoint — both the subtraction and the division cancel algebraically;
* **across a few cells** the integral is a sum of positive lengths times values
  off those same straight lines, so nothing is subtracted;
* **across many** it is a difference of two running integrals, which is O(1) and
  loses precision as the step shrinks.

The crossover **cannot be a fixed number of cells**, and finding that out cost a
measurement. The cumulative table's magnitude grows with the number of points,
so its cancellation error goes as `n / span`. With a fixed sixteen-cell
crossover, a 262144-point table used as a reference measured 29 dB of difference
against a 1024-point one — and with the antialiasing switched off the same pair
differed by 84 dB. The table was never the problem; the reference was.

The crossover is now `n/16`. Measured against the same code walking every cell,
which is exact, on a decaying note (silence cannot show an error that only
appears when the signal moves, and full level hides it behind the music):

| `walk_max` | whole note | quietest quarter |
|---:|---:|---:|
| 4 | −95.8 dB | −118.2 dB |
| 16 | −101.9 dB | bit-exact |
| **128** (n/16 at 2048) | **bit-exact** | **bit-exact** |

and the cost is flat across all of them, so there is nothing to trade.

**Silence is bit-exact**, which took one more fix than expected: the axis is
shifted at bake time so that zero lands on a grid point and the curve there is
exactly zero, but `(v - lo) * step_inv` does not come back as an integer in
floating point, so the lookup landed an ulp off the node and interpolated. A
tenth of a microvolt at the first plate is six microvolts at the second. The
index is computed as `zero_p + v * step_inv` instead.

## The three filter blocks

Everything here except two numbers is a component value, and the two that are
not are named as taste. `tube_render resp`:

| | what it is | corner |
|---|---|---:|
| F1 | V1a's input coupling, 100 nF into 10 k + 1 M | 1.58 Hz |
| F1 | V1a's cathode bypass, 0.68 µF across 820 Ω | 285 Hz, −2.93 dB |
| F1 | **taste**: the top cut (the real grid stopper corners near 19 kHz) | 9922 Hz¹ |
| F2 | V1b's coupling capacitor, 2.2 nF into 470 k | 142.4 Hz |
| F2 | V1b's cathode bypass | 285 Hz, −4.02 dB |
| F2 | **taste**: the mid lift | +4.0 dB at 700 Hz |
| F3 | V1b's output coupling, 22 nF into 1 M | 7.23 Hz |

¹ 12 kHz was asked for; at a 22.05 kHz sample rate 0.45·fs is 9922 Hz, and the
designer returns the frequency it used rather than swallowing the question.

F2's 142 Hz is not an equaliser — it is the bright cap that makes a high-gain
Marshall high-gain, and it puts the open low E's 82 Hz fundamental 9.7 dB down
before the valve that does the clipping, where otherwise it would intermodulate
with everything above it.

**Sections and not convolution**, for two measured reasons. `ag_ir` is uniform
partitioned convolution with `AG_IR_BLOCK 256`, which at 22.05 kHz is 11.6 ms
per convolution — three in series is 35 ms, a slapback echo rather than an
amplifier. And a short FIR cannot do low frequencies: 64 taps at 22.05 kHz
resolve about 345 Hz, while one period of 150 Hz is 147 samples. Every feature
this amplifier needs except the top cut is under 800 Hz. Minimum phase
throughout, because a symmetric filter rings *before* the event and pre-ringing
in front of a clipper adds an overshoot the circuit does not have.

The halfband is 41 taps, Hamming: flat to ±0.04 dB out to 8 kHz, −1.62 dB at
10 kHz, and the whole chain's latency is **30.25 samples (1.37 ms)** at ×4
against the cabinet's 256.

## What it costs to run

`tubebench bench` on the guest under `-icount`, one core of an ESP32-S3 at
240 MHz, base rate 22.05 kHz. Two valves and three filter blocks; the cabinet is
not in these — it is 981 instructions, 9.0% of a core, measured by `cktbench fx`.

| | instructions/sample | % of one core | + cabinet | products <5 kHz |
|---|---:|---:|---:|---:|
| ×1 + ADAA | 833 | 7.64% | 16.6% | −33.6 dB |
| ×2 + ADAA | 2077 | 19.08% | 28.1% | −48.2 dB |
| **×4 + ADAA** | **3997** | **36.71%** | **45.7%** | **−65.8 dB** |
| ×8 + ADAA | 7918 | 72.74% | 81.7% | −66.9 dB |

Those figures predate two things and have been re-measured since: the voicing
banks, which were ticked and never built (below), and the three-valve model. With
both banks live and the same `-icount` bench:

| | jcm800, 2 valves | slo, 4 valves |
|---|---:|---:|
| ×1 + ADAA | 929 — 8.5% | 1206 — 11.1% (3 valves) / 1448 — 13.3% |
| **×4 + ADAA** | **4155 — 38.2%** | **6094 — 56.0%** |
| ×8 + ADAA | 8081 — 74.2% | 11902 — **109.4%**, does not fit |
| of which the tone stack | +69 — 0.67% | +69 — 0.67% |

So seven biquads of fitted voicing cost about 89 instructions a sample at ×4, and
**each valve past the second costs about a thousand** — a quarter of the whole
two-valve chain each, not a half, because the up/down pair and the first filter
block are paid once whatever the topology. With the cabinet's 9% the default lands
at 47% of a core for two valves and **65% for four**, both inside one core with the
sound card and the rest of the OS still to pay for — and ×8 on four valves is the
first configuration in this file that does not fit at all.

**Eight times is not the way to spend the next instruction, and measuring it
said something more useful than yes or no.** At the original filter length ×8
came out *worse* than ×4 — −51.7 dB against −55.1 — because more oversampling
generates more genuine ultrasonic content and hands it to a final decimator with
53 dB of stopband that cannot remove it. The bottleneck was the filter. Doubling
only the outer stage bought **10.7 dB for 4.6% of a core**, and after that ×8 is
worth 1.1 dB for another 36%. Length belongs where the fold lands in the audio
band, which is the last stage; the inner ones fold above 20 kHz where nothing
listens, so they stay short.

Getting there also meant fixing the measurement. The bench drove the model with a
43 Hz tone, which moves the input a third of a table cell a sample — so every
lookup took the cheapest of the antialiasing average's three paths and the whole
benchmark measured a case that does not occur. At 990 Hz the true cost of ×4 with
ADAA was 5059 instructions, not 3564. Three lossless changes then took it to
3492: the walk crossover from n/16 to n/128 (−89.9 dB against walking every cell,
which is quieter than the table's own −88.2 dB interpolation error), a fast path
for spans that lie inside the table, and the biquad written into the chain loop
instead of called. So the session ended 21% cheaper *and* 10.7 dB cleaner than it
started.

and the pieces it is made of, so the total is explicable rather than merely
known — multiply the valve and biquad rows by the oversampling factor, pay the
rest once:

| | instructions/sample |
|---|---:|
| curve lookup | 121 |
| curve lookup + ADAA | 283 |
| three biquads | 120 |
| ×2 up + down | 312 |
| ×4 up + down | 935 |

Three things fall out of that. **The oversampling filters are a quarter of the
×4 bill** — 935 of 3564 — which is why ×2 with ADAA is the interesting row: it is
half the cost for 12 dB more products, and both fit. **ADAA costs 2.3× the plain
lookup**, more than the 1.6× the two-axis model pays, because the average has
branches where a bilinear read has none.

And **the divide is still 37 instructions.** Blocking arrived costing 70
instructions a lookup, so a skip was added for the case where the grid is not
conducting — which is nearly always — and it measured *no difference at all*.
The reason was one `/` in the branch that skipping was supposed to make cheap:
this FPU has no divider, so the decay `vc / (1 + blk_g)` cost six times what the
multiply by a precomputed reciprocal costs. With that and one duplicated index
computation removed, blocking costs 25 instructions a lookup instead of 70, and
the whole chain went from 36.0% of a core to 32.7%. docs/08 has this rake as
number one in its own list of measured primitives, and it still catches people.

For scale, the two-axis route: three valves with a tone stack at ×2 with ADAA is
4350 instructions a sample, which is 40% of a core at this rate, at −40.8 dB of
products against this chain's −68 dB under 5 kHz. Different valve counts and
different metrics, so not a like-for-like race — but the same ballpark of core,
and this one can afford ×4.

## What a build costs

`tubebench bake`, on the guest under `-icount`:

| | instructions | at 240 MHz |
|---|---:|---:|
| full build, axes fitted in four passes | 212 M | 882 ms |
| rebuild with the axes remembered | 87 M | 364 ms |

Both are preset loads, not knobs, and about what the two-axis bake costs. Two
thirds of it is the DC sweep going through the full matrix solver at about
18 600 instructions a point — reuse of `ag_ckt` buys one Koren model in the tree
and pays for it here.

The fix is identified and not made: with the cathode held and the load line
linear the sweep has two unknowns, so it could call `ag_triode_eval` directly for
about two hundred instructions a point, which would put the whole build near ten
milliseconds and make the valve's operating point a control that can be turned.
What it needs first is the host test `ag_stage` already has — the fast path
against the solver, to a millivolt — because a second piece of netlist algebra
that drifts from the first is exactly the kind of thing that gets blamed on the
model.

## Does it intermodulate — the numbers

`tube_render imd`, two tones at 1061 and 1593 Hz of equal amplitude through the
whole chain, each product relative to the stronger fundamental:

| product | drive 0.02 | 0.1 | 0.5 | 2.5 |
|---|---:|---:|---:|---:|
| 2f1 − f2, 529 Hz | −17.0 | −12.2 | −9.7 | −9.2 |
| f2 − f1, 532 Hz | −15.1 | −17.0 | −18.6 | −17.6 |
| 2f1, 2122 Hz | −18.4 | −20.5 | −22.3 | −23.6 |
| 2f2 − f1, 2125 Hz | −19.5 | −15.0 | −15.7 | −15.5 |
| 2f1 + f2, 3715 Hz | −20.7 | −12.7 | −9.6 | −9.2 |

Two things this says. The third-order products dominate the second-order ones —
two inverting stages in series cancel part of the even-order distortion, which is
the classic reason for cascading them, and the second valve is limiting nearly
symmetrically by the time anything reaches it.

And **there is no clean setting**, by construction. A 2203 has no volume control
between its two valves, which is what makes it high gain, and this model is
faithful about that: the first valve's gain of 71 puts ten volts on the second
grid from a seventh of a volt in, and the second grid's whole window is a volt
and a half. At a fiftieth of normal drive the products are still at −17 dB. If a
clean channel is wanted, the control for it is `g12` — the interstage
attenuation, which is exactly the volume pot a Plexi has there and a 2203 does
not.

## Matching the spectrum of a reference

Against a NAM capture of a Marshall on the same take, third-octave bands from
63 Hz to 6.3 kHz, everything relative to 1 kHz. `tube_render fit` renders,
compares, moves each band by a fraction of its own error, and repeats:

| | rms error | worst band |
|---|---:|---:|
| flat | 8.11 dB | 17.6 dB |
| **fitted** | **1.29 dB** | **2.7 dB** |

Four things came out of doing it, and three of them were surprises.

**One. The cabinet decides more than any control.** Before anything else was
adjusted, the reference was 11 dB above this chain below 200 Hz and 6 dB above
in the presence region — and that was almost entirely the wrong impulse. A
Marshall's cabinet is +9.6 dB at 125 Hz and flat in the middle; the Vox AC30
impulse in the tree has no low bump at all and +8.8 dB at 2.5 kHz. Swapping to
the reference's own measured response closed the whole low end to within 1–4 dB
without touching a filter. And no amount of equalising undoes it afterwards:
fitted through the Vox impulse instead, the same procedure only reaches
**2.53 dB rms against 1.29** — seven octave-wide bands cannot be a loudspeaker.

**Two. What was missing in the presence region had never been generated.** With
the cabinet removed as a variable, this chain was still 16 dB short at 4 kHz —
and raising the gain did not help (two decibels over two octaves of drive),
because its own linear response is flat and its distortion simply was not making
upper-midrange harmonics. The reference amplifier adds about 9 dB at 4 kHz over
the dry signal; this one added nothing. That is what the pre-valve bank is for
and why it lands +6 dB at 1.6 kHz: harmonics have to be **made** before anything
can shape them.

**Three. Pre-emphasis cannot supply a top end, and that is a property of
limiters.** Fitted entirely in front of the valves, the two top bands ran into
their 18 dB limit and were *still* 5 dB short. A hard limiter blanks a small
signal in the presence of a large one, so boosting a top end the guitar barely
has does not get it through — it just gets clipped away. The correction has to go
after the distortion, which is where a 2203 keeps its tone stack anyway. So the
split is physical:

| | in front of the valves | after them |
|---|---:|---:|
| 100 Hz | | +6.12 dB |
| 200 Hz | +2.70 dB | |
| 400 Hz | +2.54 dB | |
| 800 Hz | −0.45 dB | |
| 1.6 kHz | +5.97 dB | |
| 3.15 kHz | | +9.95 dB |
| 5 kHz | | +14.94 dB |

**Four. The same argument runs the other way for bass.** V1b's coupling capacitor
is 2.2 nF, cornering at 142 Hz, and that is what keeps the open low E out of the
valve that clips. Raising it to 10 nF matched the reference below 200 Hz almost
exactly — and cost 4.7 dB at 315 Hz, because more bass in front of a clipping
valve is not more bass out of it, it is less midrange. So the low end is a bass
control after the distortion too, and the capacitor stays at 2.2 nF. It is
`cfg.ccouple2` if anyone wants to hear the other choice.

The master went from 1/220 to 1/1200 doing this, which is not a scaling error:
fifteen decibels of presence on a pick attack is fifteen decibels of peak, and a
real amplifier's master does the same job.

**These numbers belong with the cabinet they were fitted through.** Change the
impulse and re-run `fit`.

## The noise in the gaps, which was the impulse

Reported on a one-stage render: an unpleasant noise under the quiet parts, and
not from the source. It was not the amplifier - the dry file measured 79.5 dB of
peak-to-floor - and it was not int16 quantisation either, though that was the
first suspect and cost a wrong fix on the way (the input scale to `ag_ir` was a
flat -12 dB regardless of level; correcting it to a measured one was right but
changed nothing here, and the first attempt at it scaled *up* by 42 dB and lost
27 more).

It was the impulse. Peak-to-floor at the same window:

| | partitions | peak/floor |
|---|---:|---:|
| no cabinet at all | — | 79.5 dB |
| Vox, 151 ms | 13 | 77.3 dB |
| **`imp_mars` cut to 200 ms** | 18 | **73.1 dB** |
| `imp_mars` cut to 100 ms | 9 | 73.0 dB |
| `imp_mars` whole, 500 ms | 44 | 61.4 dB |

Twelve decibels live in the last three hundred milliseconds, and 100 ms against
200 is a wash - so it is the tail, not the partition count. And the reason is
what that file is: the impulse response of a *nonlinear* model, where by 200 ms
the delta has decayed into the model.s own noise floor. Convolving with that
spreads it over everything. Impulses are now capped at 200 ms, which a real
cabinet never exceeds; `AG_CAB_MS` overrides it.

On the two-stage chain the same cap was worth **26 dB** (36.6 to 62.5), so this
was never a one-stage problem - it was just loudest where the signal was
quietest.

## The hiss, and where it turned out to be

Reported by ear as a constant hiss — not the crackle the two-axis model had, a
low signal-to-noise. `tube_render noise` splits the suspects in one run, and the
answer was three things, none of them the valve model:

| stage | peak | floor | peak/floor |
|---|---:|---:|---:|
| the recording | −2.4 | −77.7 | 75.3 dB |
| digital silence through the chain | −300 | −300 | silent |
| + the amplifier, drive 0.5 | −2.0 | −61.5 | 59.5 dB |
| + the cabinet | −1.0 | −65.7 | 64.7 dB |

**One. Twenty-two percent of the signal was going past the cabinet.**
`ag_ir_init` leaves `wet` at 100 of 128, and I never called
`ag_ir_set_wet`. Behind a reverb a −13 dB dry leak is inaudible; behind a
cabinet it is fatal, because the dry signal still has all of its top end while
the speaker is 17 dB down up there — so the leak is *louder* than what the
cabinet passes, and the top octave of the amplifier's noise came through
untouched. `ag_ir.h` says exactly this, in the comment on `AG_IR_WET_MAX`, and I
had not read it. Symptom: dry and cabinet renders measured 1–2 dB apart in every
band and were indistinguishable by ear.

The default no longer disagrees with that comment: `ag_ir_load` and the cabinet
presets set `wet` to `AG_IR_WET_MAX` themselves, and only the reverb presets
keep 100. Measured on this impulse, the leak was worth **15 dB at 8 kHz** —
−16.5 dB fully wet against −1.6 dB at 100, which is not a weakened roll-off but
no roll-off at all. See
[`apps/common/ir/README.md`](../ir/README.md#the-dry-leak-which-was-loud-enough).

**Two. `ag_ir`'s cabinet presets are not cabinets.** Presets 3 and 4 are one
large first tap plus twenty milliseconds of low-passed noise; the tap dominates
so completely that the response measures flat within two decibels from 80 Hz to
10 kHz. The real impulse in the tree — `1 Vox AC30 1.wav`, 151 ms, 24-bit — is
+2 dB at 2 kHz and **−17 dB at 8 kHz**, which is what a loudspeaker looks like
and is most of why a real amplifier does not hiss. `run_cab` measures whichever
impulse it loaded and says when the shape is wrong.

**Three. This chain compresses 6 dB harder than the three-valve one**, because a
2203 has no volume control between V1a and V1b, so V1b is slammed at every
setting and the recording's own floor comes up with it. That part is faithful,
and `g12` is the lever — a Plexi has a pot there. Measured on the same take and
the same window:

| | peak/floor | lookups on the flat tail |
|---|---:|---:|
| g12 = 1.0, as the circuit is | 64.7 dB | 66 176 |
| **g12 = 0.35** | **71.8 dB** | **1 670** |
| the three-valve chain in `cktbench` | 67.8 dB | — |

Nine decibels of interstage attenuation buys 7 dB of noise floor *and* takes the
second valve off its own asymptote — 66 176 clamped lookups down to 1670 — so it
is modelling a triode there rather than hard-limiting.

Two lessons worth keeping. A metric that finds its own window is not comparable
between two files: run on the other model's render, the quietest-window search
found the zero padding at the end and reported an infinite dynamic range. And a
noise floor needs its **spectrum**, not just its level: "hiss" and "rumble" are
the same number of decibels and not the same complaint, and it was the >5 kHz
share — −12.6 dB of the floor against the other model's −20.7 — that identified
the leak.

## However many stages, and presets that need no solver

A stage is a stage. The runtime never asks whether the table in front of it came
from a triode, a diode pair or a transistor — only whether there is one. So the
chain is **one to four stages**, each carrying its own filter block, and a stage
with no table is the degenerate case: its gain and its filters and nothing else,
which is what a clean makeup stage is.

The filter blocks fall out of the index rather than out of their names, and the
rule is physical: **the first stage's block runs at the base rate because it is
outside the oversampled region; every later one runs inside it**, because it sits
between two nonlinearities. Adding a stage brings its own coupling capacitor and
cathode shelf with it.

The refactor is verified the only way worth trusting: the two-stage JCM800 after
it renders **bit for bit identical** to before — −339.6 dB, which is the floor of
the comparison.

### The preset format

A preset is the whole chain as bytes — component values, voicing, and the baked
tables — and the point is what loading does *not* need: no solver, no Newton, no
axis fitting, no probe. **An application that only plays presets never links
`ag_ckt`.** Round-tripping is sample-exact, and the header carries the sizes of
the two structs it embeds so a field added to either is a readable refusal rather
than silent garbage.

Two things fall out. **The tables do not depend on the sample rate** — a curve is
a DC sweep and has no time in it — so one preset plays at 22.05 and at 48 kHz;
only the filters and the blocking constants are rebuilt at load. And **the axis
must be baked with headroom**, because it is fitted to the signal and the signal
follows the gain knob. Measured against a chain baked fresh on the material:

| axis fitted at | difference from a fresh bake |
|---|---:|
| drive 1.0 — the top of the knob | −36.6 dB |
| **drive 2.0** | **−69.2 dB** |
| drive 4.0 | −67.2 dB |

Twice the top of the range. One times is not enough because the probe is a
synthetic pluck and a real guitar drives the second grid harder than it does;
four times is worse again, because by then the axis is wide enough that the
resolution it loses costs more than the headroom it buys.

One bug worth recording, because it failed by doing nothing. The gains are cached
out of the config when the chain is built, and `ag_amp_set_voicing` did not
refresh them — so a preset played at whatever drive it was baked with, whatever
the knob said. Nothing errored; it just ignored you. There is a test for it now.

## Blending a valve with diodes

A second curve per stage - an antiparallel diode pair behind a resistor, which is
what every overdrive clips with - baked over the same axis by the same solver,
and mixed into the live tables. Opt-in per stage: `ag_amp_blend_stage`, with the
caller owning the six tables the two source curves need.

**The mix is a table rebuild, not a per-sample cost.** All three tables are linear
in it, so the blended antiderivative stays the exact integral of the blended
curve and the antialiasing average keeps working; the grid current of a clipper
is zero, so blocking fades out with the mix, which is right because a diode has
no grid. A 2048-point stage re-blends in about 50 000 instructions - a fifth of a
millisecond - so the knob can run at block rate and the audio path pays nothing.

The clipper is **normalised to the valve.s small-signal slope and inverted**, or
the knob would be a volume control: a passive clipper has a gain under one and a
12AX7 stage has seventy. Measured, the two slopes then agree within 15%, and the
diode curve is symmetric within 10% where the valve is 30% asymmetric - which is
the whole reason for having both.

Slope alone turned out not to be enough: it matches the quiet parts and not the
loud ones, because the two ceilings differ - a diode pair stops at a fraction of
a volt times the stage gain while the plate has a hundred to give - so at full
mix the diode side came back audibly quieter, a level knob pretending to be a
shape knob. So after the slope match the diode curve is **scaled once to the
same RMS over the axis** as the valve curve. The axis is fitted to the signal,
which is what makes an RMS over it a loudness rather than an abstraction; t and
h scale by the same constant, so h stays an exact antiderivative and the
antialiasing stays exact.

Two traps on the way. `a + (b-a)*w` **does not reach its ends** in floating point
when a and b are large and close, so the knob never quite arrived at either
curve; `(1-w)*a + w*b` is exact at 0 and 1 by construction. And the test that the
blended antiderivative is still an integral had a threshold tighter than float
precision at the magnitudes involved - the antiderivative runs to 1e5 in index
units, where an ulp is already 0.008.

## Turning the knobs while it plays

`tube_live` loops a take through the same chain and the same cabinet and pushes
it at the sound card, with the knobs on the keyboard. Everything else here
renders a file and asks somebody to listen afterwards, which answers a
measurement and not a question like *how much of the diode curve is too much* -
that one is answered by turning something slowly.

```bash
build-host/tube_live [take.wav]
```

Windows only, because it talks to winmm; nothing it drives is platform-specific.
Seventy milliseconds out of the card, and **counted in milliseconds rather than
in blocks** - which is a bug fix.

The card is opened at whatever rate the take has, so a fixed queue of six blocks
of 256 buys a length of audio that shrinks as the rate rises: 70 ms at 22.05 kHz,
35 ms at 44.1.  When the default take became a 44.1 kHz file the queue halved
without anyone choosing that, and the tool crackled - 35 ms across six buffers is
not enough for a polling loop that also writes to a Windows console, where a
single write can block for tens of milliseconds.  `NBUF_MS` is now the target and
the count follows the rate: thirteen buffers at 44.1 kHz, seven at 22.05.

### Torn audio, and the four things this program was not doing

It was reported as *the sound became torn, as if it cannot keep up in realtime*.
Everything below came out of that one sentence, and the first thing to say is that
the measurement offered in reply was the wrong one: it was `--dump`, which renders
as fast as it can and touches neither the sound card, nor the queue, nor the
Windows scheduler. **The render keeping up says nothing about the live path.** So
the live path now measures itself, and three things it should have been asking the
system for it was not.

**What the loop counts now**, printed on the way out and in the status line the
moment it is not zero:

```
  live path: 0 underruns, queue never below 23 of 25 blocks (134 ms),
             worst single block 3.0 ms against the 5.8 ms it lasts, startup said 33%
```

An underrun is a poll that found *every* buffer free - the card had nothing left to
play, which is the click. `--live N` runs the real audio path for N seconds and
then prints that, because until it existed there was no way to exercise the live
loop without a finger on the ESC key.

**Flush denormals to zero.** Every filter here is direct form, so a decaying note
fills the histories with numbers below 1e-38, and on x86 an operation on a
denormal costs an order of magnitude more than one on a normal number. Measured on
one second of signal followed by seven of silence: **the silence cost 24% more per
second than the signal at 22.05 kHz and 11% more at 44.1**, and with
`_MM_FLUSH_ZERO_ON` the two are identical (80 ms against 80). A program that gets
slower when there is less to hear will tear on note tails, which is exactly the
part of a guitar signal a listener is most attentive to.

**One millisecond of timer resolution.** The loop sleeps when the queue is full,
and a Windows `Sleep(1)` lasts up to 15.6 ms unless somebody calls
`timeBeginPeriod(1)` - two thirds of the whole 70 ms queue in one nap. winmm was
already linked for the sound card and that call is in it.

**Priority above the desktop.** A polling audio loop sharing a core with a
compiler loses. `THREAD_PRIORITY_HIGHEST` rather than `TIME_CRITICAL`, because
starving the console thread this program reads its keys from would be a poor trade.

**And a queue that follows the measured cost**: 70 ms while the chain costs under a
quarter of realtime, doubled past that. What has to fit in the queue is not the
render but every hiccup of the machine, and a chain eating a third of the time
makes each hiccup that much harder to catch up from. The cost line prints what it
measured on one second of the actual take:

```
  cost: 9% of realtime for the whole chain at 22050 Hz (2 stages, 4x with ADAA, float cabinet)
```

With all four in, the loop was put under six concurrent NAM renders - every core
busy - at both rates, and reported **zero underruns**, with the worst single block
at 4.0 ms of the 5.8 ms it lasts. Before them there was no counter, so there is no
honest before-and-after; what can be said is that the configuration that was
reported as torn ran a 70 ms queue at desktop priority with denormals in every
decay, and that it now takes a deliberately hostile machine to find nothing.

### The reference is per model, and level matched

`y` plays the real amplifier over the same take. It used to play the *Marshall*
render whatever model was loaded, which on the crunch and lead models put a
different amplifier in the chain and called it the reference - the one comparison
in this program that has to be against the right thing. `match` writes one per
model (`match_ref_<model>_cab.wav` when the capture had no speaker in it, so that
both sides have the same one) and `tube_live` prefers it.

And it is level matched, because it was not: measured against the chain it is meant
to be compared with, the three references came in at **+2.2 dB, +0.8 dB and
−13.7 dB**. Fourteen decibels is not a tone comparison. Matched on rms over the
first second of the take - the same material on both sides, measured in the same
pass that measures the cost - and the three now land within 0.7 dB.

Two smaller things came out of the same root, both of them constants that had
silently assumed 22.05 kHz.  The refill now runs **before** the keyboard and the
console rather than after, so a keypress cannot be heard as a crackle instead of
as the knob it was.  And the cabinet's self-timing benchmarked "one second of
audio" as a hard-coded 86 blocks, which is a second only at 22.05 kHz - so at
44.1 the one number in this program whose whole job is to say whether the
convolution keeps up with the card was reporting **half** the real cost.  It reads
290 ms per second of audio now, against an end-to-end measurement of 30% of real
time for the whole chain.

**A knob must not clear the filters, and that took two fixes rather than one.**
`ag_amp_set_voicing` resets, which is right for a tool measuring one setting
after another - the tail of the previous one is not part of the answer - and
wrong under a playing note. So the knobs go through `ag_amp_set_knobs`, which
redesigns the coefficients and leaves the state where it is. What that had to
step over:

- `ag_tube_set_adaa` and `ag_tube_set_blocking` **clear what they arm**. The
  antialiasing forgetting its previous sample is a click; the coupling capacitor
  being discharged is worse, because it is not a click but a jump in the bias -
  the stage comes back at a different operating point for the next hundred
  milliseconds. Both are lifted over the call.
- `design()` rebuilds every biquad from scratch, and a rebuilt section has an
  empty delay line. The delay line is the note that is sounding, so it is saved
  and put back section by section.

The claim is tested literally rather than by ear: a thousand samples in, an
unchanged config through the knob path leaves the next sixteen outputs **bit for
bit** what they would have been, and the same call through `set_voicing` changes
them - because a test that only checks the first half proves nothing.

Two things stay honestly discontinuous and go through `set_voicing`: the
oversampling, whose halfbands hold history at the old rate, and the number of
stages, which is a different chain.

**The staging into the cabinet follows the signal**, and that fixed a buzz
rather than a level. `ag_ir` is int16, and with a fixed input scaling sized for
a full-scale signal, a chain at drive 0.1 walks into the convolution 25 dB down -
eleven bits - and the block quantisation comes back as a buzz at roughly the
block rate, nearly independent of the note, best audible in note decays with the
top cut low. Measured with `ir_check` on the same take: error to signal
-66.5 dB going in at full scale, **-49 dB at -25 dB, and -13 dB in the decays**,
which is where it was heard. So each block is scaled to fill the sixteen bits
and scaled back on the way out; the scale rises slowly (0.35 dB a block, because
the convolution's tail entered at the old scale) and falls instantly (the
alternative to falling is clipping), bounded 30 dB above the fixed floor.
`tube_render` never had the problem because it scales by the peak of the whole
file - which is exactly the constant a live tool cannot know.

There is also a **band solo** - two bandpasses a third of an octave wide on the
very output (`k` toggles, `j`/`l` walk it in sixths of an octave) - for finding
by ear where a noise lives before believing any explanation of it.

The band solo found the staging fix necessary but not sufficient: a rattle from
2 kHz up, strongest at 3-6 kHz, louder than the signal itself above 8 kHz,
modulated by the low notes and nearly indifferent to pitch.  That is the int16
convolution's own floor, and it was measured rather than blamed: the same take
through the same impulse in double precision differs from the ag_ir render by
-24 dB relative to the signal at 0.5-2 kHz, -12 dB at 4-6 kHz, and the error
*exceeds* the signal above 6 kHz - the error is spectrally flat and a
loudspeaker's response falls off a cliff, so the top octaves are all error.
Staging can reach that floor but not lower it; the floor is the arithmetic.  So
the tool now carries the cabinet twice: a float direct convolution (4410 taps,
~80 ms per second of audio, measured and printed at startup) as the default, and
the chip's int16 path one keypress away.  **`i` switches the arithmetic and `c`
takes the cabinet in and out** - two keys rather than one three-way cycle,
because comparing two things means going back and forth between them and a cycle
that passes through silence makes that three presses and a memory test.  The
difference between the two paths IS the chip's convolution noise, on ears instead
of in a table.

**The blend knob is also the measurement.** `ag_amp_blend_stage` rebuilds a
stage's three tables from two baked curves in about 50 000 instructions - a fifth
of a millisecond against the 11.6 ms of a block - so the knob runs at block rate,
and 86 fresh tables a second under a running signal is exactly the thing that
either zippers or does not. All four chains and every stage's second curve are
baked at startup, so no keypress ever waits for the solver; that is about a
second at launch and 1.5 MB of tables.

## The fizz was the fit

A listening report of an unpleasant broadband rattle from 2 kHz up, worst at
3-6 kHz, louder than the signal above 8 kHz, present at every drive setting, and
described as *nothing a real amplifier does*.  It survived the cabinet being
switched off, so it was not the convolution; the live tool's output matched an
offline render band for band, so it was not the tool.

The measurement that settled it.  Feed a take with **nothing above 1 kHz** - a
2001-tap sinc, verified at -148 dB in the band being judged - so that every
decibel up there is something the chain made rather than something it passed.
Then compare against `ckt_exact`, which is the same two triodes in double
precision with no tables, no antialiasing average and Newton to 1e-12.  Energy
per band, relative to each file's own 200-1500 Hz:

|                          | 1.5-2.5 kHz | 2.5-3.5 kHz | 3.5-6 kHz | 6-9 kHz |
|--------------------------|------------:|------------:|----------:|--------:|
| `ckt_exact`, the reference|      -32.1 |       -43.9 |     -47.7 |   -57.0 |
| this chain, banks off     |      -31.8 |       -43.1 |     -48.9 |   -59.3 |
| plus the pre bank         |      -23.1 |       -35.1 |     -40.6 |   -49.8 |
| plus the tone stack       |      -17.7 |       -21.7 |     -23.7 |   -41.8 |

**The model is faithful to about a decibel** - tables, antialiasing,
oversampling and all.  The voicing then adds 8 dB and the tone stack another 17,
for **25 dB of gain on an intermodulation product**, and that is what an
unpleasant fizz is made of.

Why the fit did it is the lesson.  It matched the third-octave *magnitude* of a
Marshall capture and reached 1.29 dB rms, which was reported as a success - but a
magnitude fit cannot tell a dense harmonic series from sparse hash.  It only
knows how much energy belongs in a band.  The reference has that energy because
three valves and a power section make it; this chain got the same number by
amplifying what two valves made.  The note in ag_amp.h even says that the
harmonics have to be *made* before they can be shaped, and then the fit shaped
them by 25 dB anyway - which is the same mistake the note warns about, committed
one paragraph further down.

Two consequences, and the second is the real one:

- A passive Marshall tone stack **cannot boost**.  +14.94 dB at 5 kHz and
  +9.95 at 3150 are not a tone stack, they are a correction pretending to be
  one, and any refit should be bounded at or below unity gain.
- The energy has to come from **more stages**, which is what the reference has
  and what `cfg.n_stages` is for.  A third valve makes dense harmonics in that
  band; a peaking filter can only make what is already there louder.

### Neither the gain nor the shape - and a diagnostic that lied

Two hypotheses died here, and the second one died of a fault in the measuring
instrument, which is worth more than either of them.

**The gain was not it.**  Scaling both banks with `,` and `.` made the rattle
quieter and the signal quieter, together, all the way to zero.  A mechanism that
scales with the thing it corrupts is not a separate mechanism.

**The shape was not it either.**  A recording of the artefact, amplified 30 dB on
two different notes, showed a continuous oscillation at **3.9 kHz on both**, in
bursts every half period.  A harmonic cannot hold still across a change of note;
a resonance can - so the fitted tone stack's peaking sections, poles at radius
0.80 ringing 1.4 ms at 3079 and 4988 Hz, looked exactly guilty.  `ag_biq_hshelf1`
was written to replace them with one real pole at radius 0.25, and **it changed
nothing that could be heard**.

Then the band split of those recordings: energy at -1.2 dB in 3.5-5 kHz and
-20 to -34 dB everywhere below 1.5 kHz.  They were recordings of the **band
solo**, because the recorder took its samples after it.  A third-octave band pass
writes its own centre frequency into whatever passes through it, so "3.9 kHz on
both notes" was the diagnostic describing itself, and a round of work went into
chasing it.  The recorder now taps before the solo and says so when the solo is
on; the ordering is in the comment at the tap.

What survives from those files is the timing, which is real: the high-frequency
energy arrives in bursts locked to the clipping edges, twice per cycle, three
times on a note whose waveform has three.

### The humps are the artefact, and a third stage halves them

The listener's correction, and it reframed the whole hunt: **what is heard is the
humps, not what is inside them.**  In a third-octave band up around 4 kHz the
energy does not sit steadily - it arrives in humps, one per clipping edge, twice a
cycle.  That is why the rattle is high and low at the same time: the carrier is
the band, the pitch of the rattle is the hump rate, and the recorder taking its
samples after the solo is what made them visible in an editor.

`tube_render humps` measures the depth of that modulation - band pass, envelope,
the swing of it inside a 30 ms window, skipping windows where the band is quiet.
Against a NAM capture of a real Marshall on the same take, through the same
cabinet:

|                            | drive 0.5 | drive 0.1 |
|----------------------------|----------:|----------:|
| NAM Marshall, the reference|  +28.5 dB |         - |
| this chain, two stages     |  +33.7 dB |  +34.0 dB |
| this chain, three stages   |  +30.7 dB |  +29.2 dB |
| the dry take, no amplifier |  +25.8 dB |         - |

A real amplifier has humps too - the harmonics come *from* the clipped edge, so
they cannot arrive anywhere else.  What made this chain worse is that it makes
all of them in **one place**: two stages generate every harmonic at the same
instant, so they arrive as one coherent burst.  Three stages generate them in
three places with a coupling capacitor and a cathode shelf in between, the burst
spreads in time, and the depth drops five decibels - past the reference at low
drive and within two of it at working drive.

Which is the same answer the spectrum fit was reaching for from the other side,
and a better reason to take it: **the missing energy up there wants to be made by
another valve, not boosted by a filter.**  `cfg.n_stages` is 3, and `3` in
`tube_live` is one keypress - though the voicing was fitted for two, so a refit
belongs with it.

### Where the humps are born - the mechanism, in transients

Both crutches - all-pass dispersion and the multiband envelope leveller - were
built, measured, and then **removed at the author's direction**: the goal is not
to suppress the modulation but to not give birth to it.  What follows is the
mechanism, as precisely as the measurements support it, because the fix has to
come out of this description.

Take the listener's own numbers: f0 = 104 Hz, humps at 208 Hz, carrier at
3.9 kHz.  One period is 9.6 ms.  Walk through it at drive 0.1:

1.  **The edge (about 0.3 ms).**  V2's grid window - the span between cutoff and
    grid conduction - is about 1.2 V wide, and the signal arriving from V1
    swings +-6 V.  The sine crosses that window in roughly
    1.2 / (2*pi*104*6) = 0.3 ms.  During those 0.3 ms the plate slews its whole
    ~200 V, and the knees at both ends of the window put corners on the
    waveform.  **This is the only place in the entire chain where energy above
    2 kHz is created.**  Every 4 kHz component that exists was made here, in
    this 0.3 ms, with the phase this instant gives it.
2.  **The flat (4.5 ms).**  The valve is beyond the window - saturated or cut
    off - and a memoryless curve of a constant is a constant.  The output drifts
    only with the blocking capacitor's recovery, at sub-audio rates.  Above
    2 kHz the model produces **mathematical silence**: not quiet - zero.
3.  **The opposite edge**, half a period later, and the cycle repeats.

Now look at that through any band around 4 kHz.  In one third-octave there are
about ten harmonics of a 104 Hz note (h34..h44), each with the magnitude
clipping gave it (~1/n) and the phase clipping gave it - **all aligned at the
edge**, because they were all created by the same instant.  Ten comparable,
phase-locked lines spaced f0 apart sum to a pulse train at the edge rate.  That
is not an artefact on top of the signal; it IS the band's time-domain shape.
The humps and the harmonics are one object seen in two domains.

So the modulation is born from the coincidence of three things, and removing any
one of them removes it:

- **One birthplace.**  Every harmonic in the band comes from the same 0.3 ms.
  (A chain with more clipping stages has several birthplaces at different phases
  - measured, three stages read 29.2 against two stages' 34.0 on the old scale -
  and a real amplifier adds the phase inverter and the power valves.)
- **Coherent phases.**  The lines are locked to the edge.  (Dispersion attacks
  exactly this, which is why it half-worked and could not finish: decorrelating
  lines spaced 104 Hz apart needs group-delay differences approaching the hump
  period itself, milliseconds, and that much all-pass is audible as a phaser.)
- **Silent flats.**  Between edges this model makes nothing at all, so the
  valleys are empty and the swing is total.

The third point is where this model differs from reality most, and it is
measured.  In the 3.5-5 kHz band, valley level relative to the whole signal's
body:

|                        | valley-to-body |
|------------------------|---------------:|
| NAM Marshall reference |       -31.4 dB |
| the dry DI, no amp     |       -35.0 dB |
| this chain, drive 0.1  |     **-54.8 dB** |

Even the raw guitar keeps its 4 kHz band only 35 dB under the body - string
noise, pick noise, the finite Q of real vibration.  The real amplifier keeps it
at -31: its own hiss (two valves of gain lift the noise floor to audibility),
supply ripple intermodulation, the input's own top end - all of it steady,
none of it edge-locked.  This chain, fed a take with the top cut at 1 kHz, holds
its valleys twenty decibels emptier than either.  **The rattle is not that our
humps are tall; it is that our silence between them is unnaturally perfect.**

What was ruled out as the birthplace, each by measurement: the table (-68 dB),
the antialiasing, the oversampling, blocking, the axes, the int16 cabinet (the
float path kept the humps), the tone stack's resonances (shelves changed
nothing), the fitted banks' gain (scales the rattle with the signal), and the
hot-rodded V1a (5 dB).  The humps survive every one of those because none of
them is the mechanism; the mechanism is the clipping edge itself, which is also
the sound the amplifier exists to make.

### The dynamic Miller pole - the listener's hypothesis, built and measured

The listener proposed acting at the birth itself: see the clipping start and slow
the signal's growth.  The physically honest form of that idea already exists in a
triode and this model had lost it: the grid stopper drives Cgp*(1+A), and A is
the *local* gain - about 70 in the middle of the window, zero on the flats.  So a
real stage resists fast change hardest exactly in the middle of a clipping
transition, and not at all elsewhere.  Moving that RC out into a fixed linear
biquad (which is what this chain had done) keeps the magnitude and loses the
mechanism.

It was built as a one-pole in front of the lookup whose coefficient follows the
curve's local slope, with an exaggeration knob, and removed after the
measurement below.

Measured at drive 0.1, top 1000, hump depth against the reference's +8.8:

|            | hump depth | 3.5-5 kHz band rms |
|------------|-----------:|-------------------:|
| off        |   +13.6 dB |         -54.0 dBFS |
| x1 (physical)| +13.6 dB |         -54.0 dBFS |
| x4         |   +14.6 dB |         -53.7 dBFS |
| x10        |   +16.5 dB |         -54.2 dBFS |
| x25        |   +18.5 dB |         -56.5 dBFS |

**It does not remove the humps, and the reason is arithmetic that no intra-edge
treatment escapes**: the edge lasts 0.3 ms and the silence between edges lasts
4.5.  Anything that only reshapes the edge - slowing it, dispersing it, softening
it - redistributes the burst within a fraction of a millisecond and leaves four
and a half milliseconds of nothing exactly as empty as before.  Removed like the
others, same reason.

### The two edge experiments, as the listener specified them

Both live in `ag_tube_set_soft`, both find the stage's own clipping voltages
from its baked table (where the slope falls to a quarter of its centre value),
and both are knobs in `tube_live`:

- **The input knee** (`cfg.knee`, key `7`, `AG_KNEE`): a static map in front of
  the lookup - linear until 80% of the way to the stage's own clip voltage, then
  a rational approach that never crosses it.  Each polarity uses its own knee,
  because a triode's are not symmetric.  At 1.0 the valve is steered up to its
  window's edge and never through: zero clamped lookups on the whole take, and
  the 1.5-5 kHz band changes by 10 dB - this is a different distortion, softer
  everywhere, which is exactly what the composite of two curves with a softened
  join is.
- **The creeping limit** (`cfg.creep`, key `8`, `AG_CREEP`, volts per
  oversampled sample): while the lookup sits in saturation the output limit
  walks outward - 50, 50.1, 50.2 - and relaxes with a fast decay (not a reset,
  which would be a step, and a step is a new edge) when the signal leaves.  The
  flat top becomes a slow ramp, which is content between the edges.

The hump metric moves for neither (knee 1.0 reads +15.1 against the baseline's
+13.6, creep is a wash at any step), but the metric watches one band's
modulation and the knee in particular changes the whole sound - these were
built to be listened to, not to satisfy the number.

### The valves' own noise - the component that was missing

Both of the listener's edge experiments and all three of mine changed nothing a
listener could hear, and each null result narrowed the birthplace: the corner of
clipping is not it (the input knee), the flat is not it (the creeping limit),
the edge's interior is not it (the Miller pole), the phases are not fixable
(dispersion), and suppression works but is forbidden (the leveller).  What was
left standing is the one measured difference from reality: the valleys.

So the missing component: **noise, injected where the components make it**.  At
the first grid, sqrt(4kT*(rsrc+rstop+Req)) - amplified by the whole chain and
gated by its saturation exactly like the real thing - and at the last plate,
sqrt(4kT*rplate), which no saturation can gate, because it is born after the
gate.  That second path is the whole point: a saturated valve has zero gain and
chops everything arriving from before it into the same bursts as the signal, but
the plate resistor hisses *between* the bursts too.  `cfg.noise` scales both
from zero through the physical level (1) upward, `7` in tube_live, `AG_NOISE`
in a render.  White only - the flicker component dominates below a kilohertz
where the signal masks everything anyway.

Measured at drive 0.1, top 1000:

| noise      | hump depth | valley-to-body |
|------------|-----------:|---------------:|
| off        |   +13.6 dB |       -54.8 dB |
| x1 physical|   +13.6 dB |       -54.8 dB |
| x8         |   +13.5 dB |       -54.5 dB |
| x64        |   +10.4 dB |       -46.7 dB |
| x512       |    +7.4 dB |       -31.2 dB |
| NAM reference | +8.8 dB |       -31.4 dB |

Two honest readings of that table.  **x512 reproduces the reference's numbers
exactly** - both metrics, independently, to within half a decibel - which says
the reference's valley floor really is a steady broadband floor and this is the
right *kind* of component.  And **the physical level does nothing**, which says
the floor in the NAM capture is not two resistors' worth of thermal noise: it is
the capture's own noise, the power amplifier's contribution, and above all the
full-bandwidth take it was fed - the test condition here cuts the input at 1 kHz
and drops the drive to 0.1, which removes everything that fills the valleys
naturally.  x512 of thermal noise is about what a real rig's whole noise budget
looks like at high gain; whether it sounds like an amplifier or like hiss is the
listener's call, and that is what the knob is for.

### Dispersion: built, removed, restored on request, removed again

Eight all-pass sections spread logarithmically from 1.5 to 8 kHz, between the
tone stack and the output coupling, standing in for the phase inverter, the
output transformer and the cone breakup a preamp model does not have.  It moved
the metric further than anything else tried:

| | drive 0.1, top 1000 | drive 0.5, top 12000 |
|---|---:|---:|
| no dispersion    | +13.6 dB | +10.8 dB |
| 8 sections, q 6  | +11.7 dB |          |
| 8 sections, q 12 | +10.1 dB |  +8.2 dB |
| 8 sections, q 20 |  +9.0 dB |          |
| NAM Marshall     |  +8.8 dB |  +8.8 dB |

At q 12 in the default condition it reads *below* the reference.  **And on
listening it is barely noticeable** - which is the verdict that matters, and the
reason it is out of the tree for the third time.  Three and a half decibels of
measured modulation depth is not three and a half decibels of audible rattle;
this is the clearest case in the whole hunt of the metric and the ear
disagreeing, and it is worth remembering before the next number is trusted.

Two things from it are worth keeping written down.

**q 1.2 does nothing and q 12 is the floor of useful.**  A section's group delay
at its centre is about 2q/(pi*f0) - at 4 kHz, q 1.2 buys a fifth of a millisecond
against the four milliseconds between bursts, and that first version measured
*worse* than no dispersion at all.  Filling that gap needs milliseconds,
milliseconds need a high q, and a high q is where an all pass stops being
inaudible and starts sounding like a phaser.

**What it cost, measured properly, because the first attempt at this was wrong.**
Eight sections are **18.8 ns a sample** (2.35 ns a section, timed over 22 M
samples).  The chain's own per-sample cost is **1636 ns**, taken as the slope
between a 7.9 s and a 79 s render - because a single render spends **751 ms**
baking tables before it processes anything, and an estimate that ignored that
reported the dispersion at "+6%" when it is **1.1%**.  Any future wall-clock
comparison in this tree has to subtract the bake or it is measuring the bake.
On the chip a biquad costs relatively more; scaling from the guest instruction
counts in `ag_amp_defaults` puts eight sections at 4-6%.

The magnitude claim was checked rather than asserted, and the method is reusable:
the chain measured flat to **0.0002 dB** from 50 Hz to 11 kHz by
`ag_biq_chain_mag_db`, and on a real take the amplifier's third-octave levels
before the cabinet were identical to **0.00 dB** in every band with it in.  After
the cabinet the top two bands appeared to move a few dB - that is `ag_ir`'s int16
quantisation floor at -77 dBFS shifting with the peak, not a filter, and it is a
trap worth knowing about when measuring anything through the cabinet.

**And it exposed a real bug, which is fixed.**  It clicked on every knob press,
audibly, in a way nothing else in the tool does.  Not clipping - dispersion
*lowers* the peak, 0.858 to 0.746 - and not the filter, which is exactly all
pass.  `apply_cfg` lifts every chain's delay line over the rebuild that a knob
press causes, precisely so that knobs do not click; the new chain was never
registered in `AMP_CHAINS` and `chain_at`, so its eight resonators were dumped to
zero on every press while the note was still sounding.  The registry now says so
in a comment, because the failure is silent: `chain_at` returns the tone chain for
any index it does not recognise, so the count and the switch can disagree without
a warning.

### The notch: built, measured, removed

A sharp cut on the rattle's band, aimed at 4 kHz - the worst *audible* third
octave, measured as excess hump depth against the NAM capture (+5.5 dB there,
against +1.1 at 2 kHz; the excess goes on climbing to +16.7 at 8 kHz but those
bands sit 30 dB down, modulating freely with nothing in them to hear).  It went
through three versions and all of them are gone, at the listener's instruction,
because none of them removed what they were aimed at.

**Level-dependent depth did nothing.**  The window was compared against the band
level relative to the local loudness; sweeping it and asking how far the output
moved in 1.5-5 kHz against the notch switched out:

| open | close | effect |
|-----:|------:|-------:|
|  -20 |   -34 | -44.3 dB (what it shipped with: nothing) |
|  -10 |   -24 | -21.9 dB |
|    0 |   -14 | -14.3 dB |
|  +20 |    +6 | -12.8 dB (always fully in, i.e. static) |

The detector's ratio on real material sits near -10..0 dB, twenty decibels above
where the window was put, so the depth stayed at zero through everything worth
hearing.  Raising the window does not rescue it: by the time it engages during a
decay it engages always, the last rows being within 0.2 dB of static.  **The
band's level tracks the note's level too closely for a level window to tell
"music here" from "only rattle here."**  The machinery was sound - opened wide, it
reproduced the static notch to -60 dB - so what failed was the premise.

**Sharpening spared the timbre and stopped removing anything.**  At -24 dB,
static, on the same take:

|   q | rattle removed | 4 kHz band | timbre, rms over 2-8 kHz | bursts | valleys |
|----:|---------------:|-----------:|-------------------------:|-------:|--------:|
|   2 |        -3.6 dB |    -2.0 dB |                  6.62 dB |        |         |
|   8 |        +2.3 dB |    -6.9 dB |                  3.35 dB |   -6.3 |    -5.0 |
|  32 |        +2.0 dB |    -2.8 dB |                  1.11 dB |   -3.3 |    -1.3 |
| 128 |        +2.6 dB |    -0.8 dB |                  0.29 dB |   -1.1 |    +0.9 |

A **wide** notch makes the modulation worse - it takes the band's steady part and
leaves the bursts standing.  Sharp looks free (the rattle figure plateaus while
the timbre cost falls sixteenfold) until bursts and valleys are separated: at
q 128 the valleys go **up**, because a cut that narrow rings for milliseconds and
what it contributes is that tail filling the gaps.  That is smearing, already
built and discarded twice here.

So the whole family is exhausted in both directions.  Wide enough to remove the
band is wide enough to be heard as a tone control; narrow enough to be inaudible
removes nothing and only rings.  The reason is in the mechanism: the rattle is
not a line but a dense comb of about ten harmonics spaced at the note's
fundamental across the band, all bursting together, and no single notch addresses
a comb.  The listener's verdict after each version was the same - "does not
help" - and the code is out of the tree.

Two implementation notes worth keeping, because both were bugs first and both are
general: a peaking section must be **designed once and crossfaded**, never
redesigned per sample (`ag_biq_peak` resets its own delay line, so a per-sample
rebuild filters nothing at all); and a detector must compare against a **running
envelope, not a running maximum** - the first version used the peak, which after
the first loud note never moves again, so the notch was simply always on while
appearing to work.

### Against the reference, in the same band

Which raises the listener's own question - whether a real amplifier does this
too - and there is a NAM capture of one playing the same take.  The same analysis
on `ref_mars_22k.wav` against this chain at drive 0.5, both through the same
cabinet:

|                       | HF envelope peak | peakiness | envelope swing |
|-----------------------|-----------------:|----------:|---------------:|
| NAM Marshall          |          3192 Hz |  +24.1 dB |       +10.3 dB |
| this chain, cabinet   |          3208 Hz |  +32.5 dB |       +10.8 dB |
| this chain, dry        |          3214 Hz |  +10.2 dB |       +13.4 dB |

The **envelope swing is the same to half a decibel** - so the edge-locked
bursting is what an amplifier through a speaker does, not an artefact.  The peak
frequency is the same too, and it belongs to the cabinet: dry it is 22 dB less
peaky, so the 3.2 kHz is the loudspeaker's presence peak in both.

What is left is **8 dB of extra peakiness** at 3.2 kHz against the reference.
That is the whole remaining difference, and it is a spectral one rather than a
mechanism, which is why nothing that changed a mechanism has moved it.

### The knobs this hunt left behind

Scaling both banks with `,` and `.` was the obvious test and it **failed**: the
rattle got quieter and so did the signal, together, all the way to zero.  That
rules the gain out, because a mechanism that scales with the thing it corrupts is
not a separate mechanism.  What settled it was a recording of the artefact,
amplified 30 dB, of two different notes.

Measured on those two files:

- The rattle is **not impulsive**.  Highpassed above 2.5 kHz it is a continuous
  oscillation, and the burst spectrum is a single line.
- The line sits at **3.9 kHz on both notes** - 131.6 Hz and 105 Hz fundamentals.
  A harmonic cannot do that.  A filter resonance does it by definition.
- The bursts recur at **half the period** (0.475 and 0.505 of it), which is once
  per clipping edge, and the listener counted two per cycle on the low note and
  three on the higher one - the number of sharp edges a clipped waveform has.

`cfg.tone_shelf` stays, off by default: `;` in `tube_live`, `AG_TONE_SHELF=1` in
a render.  It is the honest shape for a presence control even though it did not
fix anything, and it is the only version of the top end that cannot ring.

`tube_live` records what it is playing - `0` starts and stops, writing
`build/listen/live_recN.wav` and printing the settings beside it.  Those are the
samples themselves rather than a loopback of the sound card, so there is no
resampling and no mixer gain between the artefact and the analysis.

The other knobs this hunt left behind, all in `tube_render`: `AG_NO_TONE`,
`AG_NO_VOICE`, `AG_TOP_HZ`, `AG_MID_DB`, `AG_TONE_Q`, `AG_TONE_SHELF`,
`AG_V1A_STOCK` / `AG_RCATH1` / `AG_RPLATE1`, and the `buzz` mode.

### Two things that were ruled out along the way

- **The interstage attenuator.**  At `g12` 0.3 the chain puts *nothing* above
  2 kHz - 67 dB under the signal against 49 at the default, where the floor is
  the 16-bit file's own.  But it gets there by not clipping, and putting the same
  overdrive back anywhere else brings the rattle with it, so it is a symptom
  control and not a cause.
- **The hot-rodded first valve.**  820 R / 220 k against a stock 2k7 / 100 k is
  worth only 5 dB here, and it is the point of the design rather than a fault.
  Exposed as `cfg.rcath1` / `cfg.rplate1` anyway, since a schematic is supposed
  to become a preset.

## What it cannot do yet

In order of how much each is worth:

1. **Supply sag.** A slow envelope shifting the curve, which honestly wants a
   second axis.
3. **The shape of the curve below 285 Hz.** Pre-emphasis gets the change in
   drive right but not that an open cathode also reduces the curvature. Audible
   on a palm-muted low E.
4. **The power amplifier** — phase inverter, push-pull, output transformer. Not
   in the two-axis model either, and `docs/08` says plainly that it is what gives
   a real amplifier the odd-harmonic series neither model has. Most likely the
   largest remaining difference from a real 2203.

## Three amplifiers, and what a model is allowed to claim

`ag_amp_model(&cfg, id, fs)` fills in a whole chain: which netlist every stage is
baked from, how hard each stage is driven, and the voicing. There are three, and
`AG_MODEL=` picks one in `tube_render`, `--model` in `tube_live`, and the third
argument of `tubebench bench`.

**What a model is allowed to claim, and what it is not.** The components are the
*family* each name points at, not a netlist off a schematic — argued for one at a
time, and each departure documented where it is made. What makes a chain the
amplifier on the label is the voicing fit, and **all three are fitted against a NAM
capture of the amplifier they are named for** — but the fit is the last step, not
the score:

| model | reference capture | rms error | worst band | compression: chain / capture |
|---|---|---:|---:|---:|
| `jcm800` | "Mars Gain 8", `amp_cab` | 0.97 dB | 1.8 dB | 5.6 / **16.1 dB** |
| `bogner` | Bogner Ecstasy 101B, crunch | 2.02 dB | 3.8 dB | 17.2 / 18.2 dB |
| `slo` | Peavey 5150, red channel | **0.72 dB** | 2.2 dB | 19.5 / 18.9 dB |

**Read those two columns together or neither means anything.** The crunch model
used to fit to 0.86 dB — with two valves and 4.0 dB of compression against an
amplifier that does 18.2. Rebuilt from what a Shiva is documented to be, it fits
*worse* (2.02) and behaves like the amplifier (17.2). The JCM800 is the opposite
case: it fits best of the three and is ten decibels short dynamically, because those
captures carry a power amplifier and a supply that sags and this chain has neither.
A spectrum match is what a fit can deliver; whether the thing being fitted is the
right amplifier is a question only a circuit can answer.

`ag_amp_model_fitted` reports that per model and every tool prints it beside the
name, and the flag stays even at three out of three: the next model starts
unfitted, and an unfitted chain does not sound broken — it sounds like a working
amplifier that is not the one on the label. Before the fit, the crunch model
measured 5.52 dB rms with a worst band of 10.0 dB against the real thing.

Why the two new ones fit better than the Marshall did: their captures are
**amplifier-only** (`gear_type` "amp", no loudspeaker — see the measurement in
step 1 below), so the same cabinet impulse goes on both sides of the comparison
and the choice of speaker cancels exactly. The Marshall capture has a 4×12 baked
into it, and ours had to be an impulse extracted from that.

|   | `jcm800` | `bogner` | `slo` |
|---|---|---|---|
| where the netlist is from | the tree's own 2203, twice | **a Shiva crunch channel, documented topology** | **two public analyses of an SLO-100** |
| valves | 2 | **3** | **4**, the third one cold |
| plate loads | 220k, 100k | **100k, 220k, 100k** | **220k, 100k, 100k, 220k** |
| cathode | 820R | 2k7, **820R**, 2k7 | **1k8 + 1 µF** (39k cold) |
| in, and its corner | 100 nF, 1.6 Hz | 100 nF, 1.6 Hz | 22 nF, 7.2 Hz |
| interstage couplings | 2.2 nF, 142 Hz | 2.2 nF, 142 Hz | 22 nF throughout: 7, 20, 7 Hz |
| cathode shelf | 285 Hz, −2.9 dB | 87 and 285 Hz | 88 Hz, −4.8 dB |
| stage gains | 71.7, 59.8 | 50.6, 71.9, 50.6 | 68.6, 26.6, **1.8**, 67.3 |
| the only constant left | — | **the gain pot, at noon** | **the gain pot, at noon** |
| **gain through the chain** | 66.6 dB | **92.2 dB** | **95.0 dB** |
| **compression over 20 dB in** | 5.6 dB | **17.2 dB** | **19.5 dB** |
| **its capture compresses** | 16.1 dB | 18.2 dB | 18.9 dB |
| energy above 2 kHz | −18.8 dB | −9.1 dB | −17.0 dB |
| tone stack | **yes**, 3 live pots at noon | **yes**, from a 1k follower | **yes**, values substituted |
| voicing, rms error | fitted, 0.97 dB | fitted, 2.02 dB | fitted, **0.72 dB** |
| `master` at −1 dBFS | 1/255 | 1/403 | 1/84 |
| 3.5–5 kHz humps | +10.0 dB | +11.3 dB | +7.6 dB |
| its reference reads | +8.8 dB | +9.3 dB | +6.8 dB |
| ×4 + ADAA, one core | 38.2% | **49.9%** | **56.0%** |

The three middle rows are the ones that say what each model is *for*, and they
were measured with both voicing banks switched off, on the plucked probe at drive
0.5, so they are about valves and not about equalisers. Gain through the chain is
the product of every stage's small-signal gain and the attenuation in front of it;
compression is how much of a 20 dB input range the chain swallows; the energy above
2 kHz is what the amplifier *made*, because the probe's own harmonics stop at
1.3 kHz. On all three, `slo` is the hottest and `bogner` the cleanest, which is
what a lead channel, a crunch channel and a hot-rodded Marshall in between should
measure.

**This model has been wrong twice, and both are worth recording.**

The first version had the crunch model's front end — 100k plates and its 0.55
interstage volume — and the divider in front of its cold clipper had been swept
against the hump depth of the NAM *Marshall* capture, taking the value that matched
best. It measured **19 dB less gain than the two-valve JCM800 and 1.4 dB less
compression**: a lead channel that was the cleanest model in the tree. A metric
borrowed from another amplifier optimises you into being that amplifier, badly, and
*nothing in the tree noticed*, because every check asked whether a chain worked and
none asked which chain was hotter. `test_model_gain_order` is now that check.

The second version was a family resemblance with a hand-picked 0.15 divider in
front of the cold clipper, three stages instead of four, and 10k of cold cathode
instead of 39k. What replaced it is a netlist: two independent published analyses
of the SLO-100's overdrive channel that agree value for value. **Every attenuator
between the stages is now a resistor** — the 470k series into the 500k gain pot,
the 220k into the 330k leak — and the only constant left is where the wiper sits,
which the rule puts at noon.

**And the number that says the netlist was worth having is the compression.** 20 dB
less into the real 5150 capture comes back only 1.1 dB quieter: **18.9 dB of
compression**, measured by rendering the same take twice through the capture. This
chain measures **19.5 dB**. The guessed version managed 9.8 — half the amplifier,
in the one property that a listener would call "how much gain has it got". Nothing
in a spectrum fit would have found that; it took building the circuit.

Two things this made obvious in passing. The chain's gain (95 dB) sounds absurd
until you notice the reference does the same thing, which is why "that looks too
hot" is not a measurement. And more gain is **not** more relative top end: at 95 dB
everything past the first valve is a squared wave, and a square keeps four fifths of
its energy in the fundamental, so the energy-above-2-kHz row goes the *other* way
from the captures. That gap has two named causes, both still unbuilt and both in
`ag_tube_spec_slo`: the 2.2 nF bright capacitor across the 470k divider (worth up
to 5.8 dB of treble) and the 1000 pF across the cold clipper's plate load (a low
pass at 4.2 kHz). Until they exist the matching filters are covering for them,
which is exactly the substitution this file's first section forbids — so they are
the next two things to build.

The hump row survives all of it, and gets better: `slo` reads **+7.6 dB against its
capture's +6.8**, where the guessed three-stage version was 2.5 dB over its own
reference and both two-valve models are 1.0 to 1.2 dB over theirs. Four stages make
their harmonics in four places with a coupling capacitor and a cathode shelf between
each pair, so the burst spreads in time instead of arriving all at once — which is
"The humps are the artefact, and a third stage halves them" read forwards, and it
holds at 28 dB more gain than the chain that conclusion came from.

Each model is read against *its own* reference, which is a sharper answer than the
old one-Marshall-for-everything: `slo` is 0.8 dB over its capture, `bogner` 1.0 dB
over its, and `jcm800` 1.2 dB over its own. What is left in all three is a power
amplifier and a supply that the model does not have at all (the list in "What it
cannot do yet"), and that is where the next decibel is, not in another filter.

### The crunch model

A **Bogner Shiva's crunch channel**, which the places that hold both schematics
describe as "a standard Marshall 2203 specification with modifications for higher
gain: three gain stages plus a cathode follower, the second gain stage much hotter
than a standard 2203". So the topology and the base values are documented, and
`ag_tube_spec_bogner` says which of its three departures from a 2203 is an
interpretation of that sentence:

| stage | plate / cathode | what it is |
|---|---|---|
| V1a | 100k / 2k7 + 0.68 µF | the stock 2203 front end — documented |
| V1b | **220k / 820R** + 0.68 µF | "much hotter", turned into the two values that make a 12AX7 hotter — **the interpretation** |
| V2a | 100k / 2k7 + 0.68 µF | the third stage the Shiva adds; that it exists is documented, its values are the amplifier's own standard |

Everything else is a 2203: 100 nF and a 68k stopper into a 1M leak at the input,
2.2 nF interstage couplings against 470k grid leaks, the tone stack after the
follower. The **cathode follower is not modelled** and what that costs is stated
rather than hidden — a follower is a buffer, so nothing in level, but it drives the
tone stack from about a kilohm instead of a plate's forty-eight, and a passive
network's response depends on what drives it. `ag_amp_tone_spec` therefore gives
this model's stack a 1k source: the buffer is represented by the only thing about it
the rest of the chain can tell.

**Why three stages, and why the two-valve version was wrong.** The same measurement
that rebuilt the SLO: render the take through the capture twice, twenty decibels
apart, and see how much of that comes back.

| capture | its own compression | this chain |
|---|---:|---:|
| Mars Gain 8 (JCM800) | 16.1 dB | 5.6 dB |
| Bogner Ecstasy, crunch | **18.2 dB** | **17.2 dB** |
| Peavey 5150, red | 18.9 dB | 19.5 dB |

The two-valve crunch model managed **4.0 dB** against the amplifier's 18.2. It had
been built the other way round — a "crunch channel" assembled from the idea that
crunch means less gain, with a stock front end, an invented 4.7 nF coupling to make
it fat and a 0.55 volume dialled until it sounded like the word. What the amplifier
is turns out to be a **26 dB hotter chain than the two-valve Marshall**, and no
spectrum fit would ever have said so: the fit was already at 0.86 dB rms with the
wrong number of valves.

Two things worth taking from that. A word — "crunch" — is not a specification, and
using one as if it were is how three of the numbers in this file came to be
invented. And **the JCM800 model is the one now visibly short**: 5.6 dB against its
capture's 16.1. That gap is honest rather than a bug, because those captures contain
a power amplifier and a supply that sags and this chain has neither; the other two
models reach the figure with preamp gain alone, which matches the number without
matching the mechanism. Worth knowing before 17.2 is read as a triumph.

### The cold clipper, and what it needed from the bake

`slo`'s third stage has **39k of cathode resistance and no bypass capacitor**,
which is what both published analyses of the SLO-100 say and what makes a lead
channel sound like one. Measured on the baked curve:

| grid | plate swing | local slope |
|---:|---:|---:|
| −24 V | +9.0 V | 0.00 |
| −8 V | +9.0 V | −0.01 |
| −4 V | +6.7 V | −1.43 |
| 0 | 0 | −1.77 |
| +4 V | −7.4 V | −1.89 |
| +12 V | −22.8 V | −1.94 |
| +24 V | −46.3 V | −1.97 |

**A gain of under two, and one hard end.** Four volts under the bias it is already
cutting off and by eight it has stopped dead — nine volts is all the positive swing
the plate has left, because 39k of unbypassed cathode holds the valve nearly shut.
The other side is straight to within ten percent for twenty-four volts and never
draws grid current. So the stage is a one-sided limiter with almost no gain, sitting
between two stages that have sixty-eight each: it is there to shape, not to amplify,
and on the take its grid sees swings of a hundred volts against a five-volt window.

The earlier guesses were 22k and then 10k, on the reasoning that a stage with a gain
of two "cannot be earning its place". It is: the published amplifier does exactly
that, and the compression measurement says the chain built this way lands within
0.6 dB of the real one. The reasoning was the thing that was wrong, not the value —
which is the argument for reading a schematic rather than arguing about one.

For the record of what the guesses cost: unbypassed, the gain is 12.4 at 10k, 3.3 at
22k and 1.8 at 39k, and cut-off arrives about five volts under the bias whatever the
resistor is. Two of those three numbers were chosen by an argument about whether a
stage "earns its place"; the third came off a schematic.

Two things had to be fixed before that stage was a valve rather than a rumour,
and both failed quietly:

- **The curve was swept with the cathode held at its bias.** That is right for
  every stage with a bypass capacitor, because the difference between held and
  free is the first-order shelf `design()` puts in front of the table — and with
  no capacitor there is no shelf, so the same sweep hands back a stage with seven
  times its real gain and a knee in the wrong place. `ag_tube_bake` now sweeps an
  unbypassed stage with its cathode resistor in circuit. It would have looked
  entirely plausible.
- **`fit_range` fell through to ±5 V.** It locates saturation by sweeping the grid
  to +4000 V, which on an unbypassed stage is far outside anything the valve model
  was fitted over: Newton gives up, what comes back is the cut-off plate voltage,
  the apparent swing is negative, and the fit takes the branch it keeps for a
  curve with no slope at all. A cold clipper baked over ±5 V stops a tenth of the
  way up its own linear region and clamps *both* halves of the wave. It showed up
  as 18% of all lookups off the end of the table and in no other way. Now the two
  extremes are taken as the largest and smallest values the probe actually
  reached, and the saturated end is found walking up rather than down — on a
  monotone curve those are the same answer, and this one does not depend on the
  last probe point being trustworthy.

### The voicing banks did nothing at all

Worth its own heading, because it is the largest thing this found and it made no
sound. Both banks — the one in front of the valves and the tone stack behind them
— were initialised, reset and ticked, and **nothing ever pushed a section into
either chain**. Every band in `cfg.voice` and `cfg.tone` was ignored, and an empty
biquad chain is a unity gain, so the amplifier still worked and simply had no
voicing. The numbers looked right where they were read; what was missing was the
code that turned them into filters.

Two measurements, either of which would have caught it the day the fit was
recorded:

- A render with `AG_NO_TONE`, which zeroes the entire post bank, came back
  **bit-identical in every third-octave band** to one with the fitted bank.
- `fit` asked to score the stored answer without moving it reported **7.69 dB rms,
  worst band 16.3 dB**, where the fit that produced those numbers had recorded
  1.29 dB rms and 2.7 dB.

With the banks built, the same command reports **1.27 dB rms, worst 2.7 dB** —
which is the fit's own recorded result, reproduced. And the JCM800's stored
`master` of 1/1200 now measures as the value that peaks at −1 dBFS (1/1156),
which is what the note beside it always said it was: "it went from 1/220 to 1/1200
when the tone stack was fitted". Both numbers had been describing a filter chain
that was not there. `test_voicing_banks` is the regression test.

## Where post-stage matching goes: a bank, or the impulse

The architecture at the top of this file allows exactly one place after the stages
and it is the IR. The code has a bank of biquads there instead. So the two were
built and measured against each other, on `bogner` against its own capture, with
everything else held still.

Three candidates, and the third is the one the architecture asks for:

- **A — a bank in the audio path.** Where the code is now: `cfg.tone[]`, three
  sections at the base rate, then the cabinet convolution.
- **B — the same bank folded into the cabinet's impulse.** Everything from the
  last plate on is linear, so `chain → eq → cab` and `chain → (cab ⊛ eq)` are the
  same filter. `tonefilt` makes the combined impulse.
- **C — the impulse fitted directly.** `irfit` measures our render against the
  reference band by band and builds the correction as a cascade of gentle peaking
  sections, then convolves it with the real cabinet impulse. No bank anywhere.

| | A: bank | B: bank folded in | C: impulse fitted |
|---|---:|---:|---:|
| third-octave rms vs the capture | **0.86 dB** | 0.90 dB | 1.21 dB |
| worst band | 2.1 dB at 63 Hz | 2.3 dB at 63 Hz | 3.3 dB at 6.3 kHz |
| on a take it was **not** fitted on | **1.48 dB** | — | 2.65 dB |
| peak to noise floor | **73.6 dB** | 71.1 dB | 70.0 dB |
| 3.5–5 kHz hump depth | +10.3 dB | +10.4 dB | **+9.3 dB** |
| cost in the audio path | 92 instr, 0.84% core | **0** | **0** |

For scale: with no post-stage matching at all the same chain is 5.80 dB rms from
the capture, so all three are doing most of the work.

**What each row says.**

**B is A.** 0.90 against 0.86, and the octave spectra agree within 0.5 dB
everywhere — which is what "the same filter" has to look like when it is measured
rather than asserted. The fold needs no extra length either: 200 ms and 300 ms
give the same render, because the correction's own ring fits inside the cabinet's
window, and the tail past 200 ms is 55 dB down.

**But B is not free, and the reason is arithmetic rather than theory.** It costs
2.5 dB of noise floor. In double precision the two paths agree to −58.9 dB; through
`ag_ir` they agree to −27.1 dB, worst in the top octave. `ag_ir` takes the impulse
as int16 and works in block float, and the bank folded into it is a rising tilt
that reaches **+25 dB at 8 kHz** — a first-order shelf keeps climbing past its
corner, so most of that gain sits above the last band anybody fitted, in a region
where the speaker is 28 dB down. Amplifying an int16 impulse's own quantisation
noise by 25 dB is the whole of the 2.5 dB.

**C is what the architecture wants, and it wins the row that the rattle hunt cared
about.** It lands on the capture's hump depth exactly — +9.3 dB against the
capture's +9.3, where both bank paths sit a decibel over. Its correction is
+6 dB in the bottom two octaves, flat through the midrange and +10 dB up top,
realised as 24 gentle overlapping sections instead of two shelves and a peak, and
smooth minimum-phase gain does not ring the way a shelf does.

**And C is behind on accuracy today: 1.21 dB against 0.86, and 2.65 against 1.48
on a take neither was fitted on.** Both of its weak spots are the same band, the
top third-octave, and both come from the same place as B's: an impulse carrying
gain, quantised to int16, convolved in block float. The fix is not another filter -
it is the precision of `ag_ir`, which is already on record as the limit here
(−60 dB at 4–6 kHz, wanting int32 spectra).

So: **the bank is more accurate today, the fitted impulse is architecturally right,
cheaper, and closer on modulation depth.** The way to have both is to raise the
convolution's precision rather than to choose.

### That last sentence has since been measured again, and it came out the other way

Everything above was measured a file at a time, with the impulse fit driven by
whichever renders happened to be on disk. `match` now does the whole thing in one
process for all three models, which means it can render both architectures on the
same take and measure both against the same reference in the same run - and doing
it that way, **the fitted impulse is closer on every model**:

| against its own capture, third-octave rms 63 Hz - 6.3 kHz | A: the bank into the cabinet | C: the fitted impulse alone |
|---|---:|---:|
| `jcm800`, Mars Gain 8 | 1.48 dB | **0.71 dB** |
| `bogner`, Bogner Ecstasy | 2.67 dB | **0.61 dB** |
| `slo`, 5150red | 0.83 dB | **0.67 dB** |

Which is not surprising once said out loud: the impulse fit has 24 overlapping
bands to spend and the bank has seven fixed sections, three of which are spent on
the pre-valve side. The earlier 1.21-against-0.86 was one model, one pair of files,
and the pipeline that produced the files had a trap in it - see below.

**The trap, because it is worth more than the numbers.** `irfit` fits the
*residual* between two signals that both already have a loudspeaker in them, and
then convolves that residual with the cabinet to make one impulse carrying both.
Hand it our render **without** the cabinet and the residual comes out containing
the whole speaker response, which is then applied a second time. Measured: the
fitted impulse played **+17 dB at 100 Hz and −12 dB at 4 kHz** against the bank it
was supposed to replace - a 4x12 heard through a second 4x12 - and the impulse fit
still reported a respectable 0.84 dB rms, because a fit can always agree with
itself. It was caught by the A/B in `tube_live` sounding absurd, not by a number.
The same mistake was then made *again* an hour later in the code that renders the
two architectures for the table above, which is why both places now say in a
comment which of the two renders they need.

### Hearing the two modes, on one key

The tables above are third-octave rms, and no amount of that settles what the two
modes sound like. `tube_live` holds both at once — mode 1's impulse with the bank in
and mode 2's without it — and `z` swaps them together, under a note that keeps
playing:

```bash
argon match "assets/audio/guitar-di/Bogner.nam" -Model bogner
build-host\tube_live.exe --model bogner build/listen/tube_di_22050.wav
```

The first command is what makes the second one able to do it: it writes the pair of
impulses the key needs - `ir_<model>_bank.wav` for mode 1 and
`ir_<model>_fitted.wav` for mode 2, both fitted around the same speaker - and
`tube_live` prefers them over its own defaults for exactly that reason. Two
impulses carrying two different speakers would make this key a comparison of
loudspeakers. `match_cab_<model>.wav`, the raw cabinet, is still written and is what
slot 0 falls back to when mode 1's impulse has not been made yet.

With no fitted impulse present the key still works and does something else: it
takes the post bank out and puts nothing in its place, leaving the cabinet alone. It says so, in both the
line it prints and the status flag (`noPost` rather than `IRfit`) - which it did
not, at first: it announced "the fitted impulse, carrying it alone" and then added
that no impulse had been found, two lines that contradicted each other.

What moves and what does not: the post bank comes out and the impulse comes in.
The **pre**-valve bank and the mid lift stay, because they decide what gets
distorted and cannot be moved behind the valves, and so does the passive tone
stack, which is a circuit out of the schematic rather than a correction. The
status line says `IRfit` while the impulse is carrying it.

Two things had to be right for the switch to be a question about tone.

- **Level, and it took two attempts.** Both impulses are peak-normalised, and a
  peak says nothing about how loud a cabinet is, so the first attempt matched the
  impulses' own **energy**. That is the same thing only if the signal is white, and
  a guitar is the opposite of white - most of its energy sits in two octaves around
  200 Hz. What the error sounded like, reported by a listener before it was
  measured: *with the baked-in biquads it is quieter and duller*. It was quieter,
  and not even consistently: **2.7 dB down on `jcm800`, 5.7 dB down on `bogner` and
  2.3 dB up on `slo`**, so no single volume knob would have taken it out.

  What it is matched on now is the audio: one second of the take rendered through
  each of the two architectures, by the same `render` the audio loop calls, and slot
  1 scaled to equal mean square. The three now land within **0.36 dB**. A level
  difference is the one thing an A/B must not have, because loudness wins every
  comparison it is allowed into.
### What the key sounds like now, and what it sounded like before

Before both sides were fitted, `z` compared the shipping bank into a raw cabinet
against a fitted impulse, and the difference was large and uneven:

| | unmatched side vs the amplifier | fitted impulse vs the amplifier | worst single band between the two | what a listener heard |
|---|---:|---:|---:|---|
| `jcm800` | 1.56 dB rms | 0.72 dB | 3.3 dB at 5 kHz | "a big difference" |
| `bogner` | 2.70 dB rms | 0.63 dB | 4.7 dB at 200 Hz | "a big difference" |
| `slo` | 0.81 dB rms | 0.69 dB | 1.0 dB | "no difference at all" |

The ear ranked the models exactly as the measurement did, biggest on `bogner` and
nothing on `slo` - and both were reporting the same thing: **how much the shipping
bank alone was missing**, which is a real and useful measurement but not the
question the key was built to ask.

With both modes fitted, on the same take through `tube_live`'s own output:

| | mode 1 vs the amplifier | mode 2 vs the amplifier | between the two modes |
|---|---:|---:|---|
| `jcm800` | 0.72 dB rms | 0.72 dB | 0.54 dB rms, worst 1.1 dB at 315 Hz |
| `bogner` | 0.59 dB rms | 0.65 dB | 0.28 dB rms, worst 0.8 dB at 63 Hz |
| `slo` | 0.33 dB rms | 0.71 dB | 0.52 dB rms, worst 1.6 dB at 63 Hz |

Which is a real A/B: a difference of half a decibel, in the bottom two octaves on
two of the models, with both sides within three quarters of a decibel of the
amplifier.

**Two measurement notes that came out of getting this wrong.** The numbers above
were computed twice through unrelated paths - inside `tube_render` from its own
renders, and from `tube_live`'s output files - and agree to a tenth of a decibel.
And the difference must not be quoted as the *level of the residual*, which is how
it was quoted at first: by that measure `slo` had the largest difference of the
three and `slo` is the one nobody could hear. A residual mixes magnitude with phase,
and phase costs nothing to the ear while costing everything to a subtraction. **The
band table predicts audibility; the residual does not.**

### Two fitted modes, and the pairing that made this look like a big question

`match` writes **two finished configurations** for the post-valve correction, not
one:

| | the bank of biquads | the impulse |
|---|---|---|
| **mode 1** | stays in, as it ships | fitted to whatever the bank did not manage → `ir_<model>_bank.wav` |
| **mode 2** | at zero | fitted to carry the whole correction → `ir_<model>_fitted.wav` |

`z` in `tube_live` swaps those. Which is what should have been compared from the
start: the first version of that key put the bank into a **raw, unfitted** cabinet
on one side and a fitted impulse on the other, which compares a matched chain with
an unmatched one. It measured as a large difference and it sounded like one -
correctly, and for the wrong reason.

**With both sides fitted, the two modes are nearly the same**, which is what a
listener predicted before it was measured, and for the right reason: both are linear
and both sit after the last plate, so the same total response is reachable either
way.

| third-octave rms vs the amplifier | fitted take | held out (`GuitarClean1`) |
|---|---:|---:|
| **as it ships**: the bank into a raw cabinet, `jcm800` / `bogner` / `slo` | 1.48 / 2.67 / 0.83 dB | 3.95 / 2.28 / 1.80 dB |
| **mode 1**: the bank and an impulse for the remainder | **0.70 / 0.55 / 0.34** | 3.51 / 1.97 / 1.82 |
| **mode 2**: one impulse carrying all of it | **0.71 / 0.61 / 0.67** | 3.30 / 1.95 / 1.75 |

So: **both modes beat what ships today by 0.8 to 2.1 dB, and they differ from each
other by 0.01 to 0.33 dB.** Held out, the two are within 0.21 dB of each other on
every model. There is no accuracy argument between them worth having.

**Then can it be claimed that baking the biquads into the IR is more accurate?**
No, and the question is worth separating into two.

*Baking the same biquads in* changes nothing at all: everything from the last plate
on is linear, so `chain → bank → cab` and `chain → (cab ⊛ bank)` are the same filter
- measured 0.90 dB against 0.86, a wash - and through the chip's int16 convolution
it is 2.5 dB *worse* on noise floor, because a folded impulse carrying +25 dB of
tilt spends its 16 bits on the tilt.

*Fitting the impulse* is what buys the 0.8-2.1 dB in the table, and it buys it in
either mode. What is left to choose between them is not accuracy:

- **Arithmetic on the chip.** Mode 1 keeps the gross shaping in seven biquads,
  which are exact, and asks the int16 impulse only for the remainder; mode 2 puts
  the whole tilt into the impulse, and an impulse carrying a large tilt spends its
  16 bits on it - the 2.5 dB of noise floor measured on the folded-bank impulse
  above.
- **Dynamics, which neither touches.** Both are linear after the last plate, so
  their compression figures are *identical*; that is settled in front of the
  valves, not here.

  **The int16 convolution itself is not a reason to prefer either, and this file
  said it was.** It claimed `ag_ir` sits 1.18 dB rms from an exact convolution,
  worst −2.1 dB at 200 Hz. That number is real and it is not the convolution: a
  **delta** through the engine comes back at **0.00 dB in every third-octave band**,
  and so does **pink noise**. The 1.18 dB appears only on material with decays in
  it, because `run_cab` stages the input with one scale for the whole take, so a
  note sixty decibels down enters eleven bits down and comes back with flat
  quantisation noise on it - which, normalised to 1 kHz, reads as a tilt. The
  filter is exact; the level going in was not.

  Giving `run_cab` the scale-follows-the-signal staging `tube_live` uses made the
  measurement **three times worse** - 1.37 dB rms on pink noise, +6.1 dB at
  6.3 kHz, where the fixed scale had been exact - because raising the scale
  rescales the new input while the convolution is still summing the tail of blocks
  that entered at the old one, and a cabinet keeps its low end in that tail. Live
  that trade is worth it (quantisation buzz is more audible than a decibel of
  tail); in an analysis path it is not. So `run_cab` keeps the fixed scale, and
  everything the fit and the verdicts measure goes through `convolve_f` - an exact
  FFT convolution in double, which is also what made this measurable at all.

- **Knobs, which turned out not to be an argument.** This file said mode 2 leaves
  no tone control after the valves. It does: the passive Bass-Mid-Treble stack is a
  *circuit* and sits after the last plate in both modes, and the matching bank is a
  fit that nobody turns. Mode 2 removes only the post-stage *matching* bank, and
  the pre-stage matching filters - which are what decide the character of the
  overdrive - are untouched in both.

**And one caveat about all of the numbers.** Everything is 1 to 3 dB worse on
material it was not fitted on, which is larger than any difference between the
modes, so nothing here is accurate in an absolute sense yet - the take-to-take
spread belongs to the model, not to the impulse. `match` therefore always runs the
held-out check (`AG_EVAL_DI` names the take) and says so when there is none, rather
than letting a fit report on its own material. Note also that `E2_s1_01`, the first
take used for it, is a **single sustained E2**: its third-octave spectrum is a
harmonic comb with the model's own hash in the gaps, which is a poor target for this
metric.

### Why `bogner`'s bank is the worst of the three

Because of where its error is: the bank is **4 to 5 dB short between 160 and
400 Hz**, and that is not a fitting accident, it is the architecture. `fit` gives
200-1600 Hz to the **pre** bank, on the grounds that in front of the valves is
where it is decided what gets distorted; what is left for the post bank is the
bottom and the top, and `bogner`'s shipping post bank is three sections - +9.65 dB
at 100 Hz, +9.14 at 3150, +1.94 at 5000 - with **nothing at all between 200 and
1600**. The Q-1 peak at 100 Hz reaches 140 Hz and stops.

So the low mids can only come from in front of the valves, and there they cannot:
more bass into a clipping grid is not more bass out of it, it is less midrange -
this file records 10 nF in V1b's coupling capacitor buying the bottom end and
costing 4.7 dB at 315 Hz. An impulse after everything has no such constraint,
which is exactly the argument the architecture at the top of this file makes. On
`bogner` that argument is worth 2 dB rms.

- **The sample rate, which is now the default rather than advice.** At 44.1 kHz
  this program costs four times what it costs at 22.05 - twice the samples through
  the chain and twice the cabinet taps - and 22.05 is what `apps/amp` runs at, so
  the 44.1 form is more expensive *and* less faithful. Measured end to end on the
  same chain: **9% of realtime at 22.05 kHz against 37% at 44.1**. The default take
  is `tube_di_22050.wav`, with the misnamed 44.1 kHz `tube_di_22k.wav` as the
  fallback, and every run prints what it measured with a warning past 55%.

  That line exists because a report of torn audio has exactly one first question -
  *was it keeping up?* - and nothing on the screen used to answer it. What was on
  the screen was worse than nothing: the cabinet's own cost figure was measured on
  a benchmark that fed its own output back scaled by 1e-9, so after one block every
  multiply was on a denormal. It printed **508 ms per second of audio at 44.1 kHz
  where the whole chain including that convolution measures 398** - a tool built to
  say whether the program keeps up, wrong by a quarter, in the pessimistic
  direction.

- **Bandwidth, which nearly made the first listen a lie.** A fitted impulse carries
  the correction, so above its own Nyquist it carries *nothing*, while the bank on
  the other side of the key keeps working. `ir_bogner_fitted.wav` is 22.05 kHz
  because that is the rate `apps/amp` runs at. Played against the bank on the
  44.1 kHz take the two differ by 5.9 dB and the top two bands are −64 dB against
  −36 — most of which is missing bandwidth, not architecture. On a 22.05 kHz take,
  where the impulse is native and both sides stop at 11 kHz, they differ by 9.6 dB
  and the fitted side is +2.4 dB at 1.6–2.5 kHz and −13 dB at 10 kHz. The tool
  prints the warning when the impulse is the slower of the two;
  `build/listen/tube_di_22050.wav` is the take that avoids it.

### What irfit had to get right, and got wrong first

Two bugs, both of which produced a confident number and a wrong filter. They are
worth recording because both are the same mistake in different clothes: **a fit can
only ever agree with its own analyser.**

- **The analyser has a tilt that only shows on an impulse.** A third-octave band is
  proportionally wide, so a flat impulse measures 3 dB more energy per octave than
  the one below it. On two signals the tilt cancels in the difference, which is why
  `fit` never had to know; on an impulse it does not, and the first attempt spent
  its iterations equalising the analyser - +20 dB at 100 Hz with alternating signs
  between neighbours. Measuring a bare delta once and subtracting that baseline is
  the whole fix.
- **The target was the reference's spectrum instead of the correction.** The
  difference `target − ours` went into the initial gains and then the loop kept
  fitting `target` itself. It converged, reported **1.29 dB rms**, and rendered
  **6.65 dB** from the reference - worse than no correction at all, which is the
  only reason it was caught. The check that catches this class is now printed every
  run: the last two columns of the table are what the finished impulse measures
  against what was wanted.

One more thing the fit needs, for a reason that is not obvious. The correction's
**overall level is not information** - the impulse is normalised on the way out and
the render trims to −1 dBFS after that - and acting on it makes the loop unstable:
both spectra are relative to their own 1 kHz, so lifting the bands around 1 kHz
lifts 1 kHz through their skirts, which reads as every other band having fallen. It
improved to 1.82 dB by pass 12 and was back at 8.07 by pass 29, gains climbing. Take
the mean error out before applying it and it converges monotonically to 0.49 dB.

## The tone matcher: one command from a capture to a voicing

```bash
argon match "assets/audio/guitar-di/Bogner.nam" -Model bogner
```

Everything below used to be five commands, two of which were easy to get wrong in
a way that produced a confident number: resample the take to 48 kHz, run a Go
binary that lived in a temporary directory, resample back, `cabify` the result if
the capture had no speaker in it, then fit. Now it is one, and the two decisions
that used to be manual are measured instead.

**What it does, in order.** The take goes up to the capture's own sample rate -
which is not negotiable, since every filter a capture learned sits at a fixed
fraction of it. It plays the capture twice, at the fitting level and 20 dB down.
It brings the result back down to the take's rate and writes it to
`build/listen/match_ref_<model>.wav`, which is what `irfit` wants as its target and
what to listen to beside a render. It decides what cabinet the comparison needs.
It fits both voicing banks. Then it measures the compression of the capture and of
the chain, side by side.

All of it in one process, in float: the reference the fit is measured against no
longer has two 16-bit quantisations on it from files written and read back.

**What it decides for you, and how.**

- **Whether the capture has a loudspeaker in it**, from the reference's own
  third-octave spectrum: a twelve-inch guitar speaker is 15 to 25 dB down at
  6.3 kHz relative to 1 kHz and a preamp is not. The captures here do not sit
  anywhere near the threshold - `Mars Gain 8` measures −16.1 dB there, `Bogner`
  −0.4 and `5150red` +0.2 - so the rule is a decision and not a guess. Metadata is
  not used: half of these files have none, and `gear_type` is worth what the person
  who typed it was worth.
- **The cabinet.** A capture with a speaker gets one on our side too, taken from
  the capture itself the way `imp_mars.wav` was - a delta in, 200 ms out, written
  to `build/listen/match_cab_<model>.wav` so it can be looked at. A head-only
  capture gets **none on either side**, which is the same fit as putting the same
  impulse on both (a common linear filter cancels exactly in a band-by-band
  difference) and is cheaper by the whole of `ag_ir`'s int16 convolution noise -
  which is worst in precisely the top bands the comparison cares about. `AG_CAB_IR`
  still overrides, and if it is set against a head-only capture the tool says
  loudly that it is about to make the voicing correct a loudspeaker.

**And both architectures, not one.** The architecture at the top of this file asks
for an impulse after the stages, and the code has a bank of biquads there, so
`match` produces *both* answers from the same run: the seven-section bank as C
literals to paste into `ag_amp_model`, and `build/listen/ir_<model>_fitted.wav`,
one impulse carrying the correction and the cabinet together. It also renders both
and measures them against the same reference, because "architecturally right" and
"sounds more like the amplifier" are different claims:

```
  Against the same reference, third-octave rms from 63 Hz to 6.3 kHz:
    the biquad bank into the cabinet    1.48 dB
    the fitted impulse alone           0.71 dB
```

The impulse is fitted against the chain **as it ships**, not against the voicing
the same run just proposed - those are two answers to one question and only one of
them is in the tree, and the impulse exists to be A/B'd against what is compiled
in today. Adopt the printed numbers and re-run, and the impulse follows.

**What it prints, and which two numbers matter.** The band table and its rms are
the fit. The two lines under it are the check on the fit:

```
  Compression - the same take 20 dB down, and how much of that came back:
    the capture        16.1 dB
    this chain         16.0 dB   (-0.1 dB)
```

A third-octave fit will match the spectrum of an amplifier whose dynamics are
nothing like the model's, and that mistake has been made in this tree twice. So
this is printed every run, after the fit rather than before, and more than two
decibels apart gets a paragraph saying so. It reads as it does above because pass
one now fits *for* it - see below; before that pass existed the same model measured
11.6 dB against the capture's 16.1.

### The front first, and against dynamics rather than against the spectrum

The order matters and it took two tries to get right. The filters **in front** of
the valves decide what the distortion *is* - how hard the valves are driven, and so
how much it compresses and how much grain it makes. The bank **behind** them decides
tone. Fitting both at once, each band assigned to one bank or the other by
frequency, lets the bank behind absorb the very error that should have been moving
the one in front: the spectrum converges and the character stays wrong.

So `match` fits in two passes, and the first one is not judged on the spectrum at
all:

```
  PASS 1 - the front, against the amplifier's compression (16.1 dB)
    lift below 1 kHz   compression   made above 2 kHz
     +0.00 dB           12.2 dB      -11.7 dB (capture -16.4)
     +2.00 dB           14.3 dB      -11.8 dB (capture -16.4)
     +4.00 dB           16.0 dB      -11.8 dB (capture -16.4)
  PASS 2 - behind the valves, with the front frozen
```

One parameter - a lift across 100 Hz to 800, which is the drive into the valves -
against one number, the compression the amplifier does. Well posed, three
iterations, and bounded to ±6 dB because six decibels in front of three cascaded
valves is already a different amplifier. The sensitivity it steps by (0.18 dB of
compression per dB of lift) is measured, not assumed: see `sens` below.

Judged on the spectrum instead, pass one **diverged** - 3.66 dB rms to 6.19 over
eight iterations, even when scored only on the bands it was allowed to move -
because raising the midrange in front of a clipper raises the clipper's output in
the same bands, so the shape barely moves while the compression grows.

**And it overturns something this file has said twice.** The `jcm800` was 4.5 dB
short of its amplifier's compression, and that was written down as *the missing
power amp and sag, which no filter can fix*. Four decibels of lift in front of the
two valves takes it from **12.2 dB to 16.0 against the capture's 16.1**. The
dynamics were reachable from in front all along.

What is *not* fixed, and is the more interesting half: the same chain makes
**−11.8 dB of its output above 2 kHz where the amplifier makes −16.4** - 4.6 dB more
grain than the real thing, and the lift does not move that number at all. So the
JCM800 model now squashes like a JCM800 and is still coarser than one. That is the
`jcm800` line of work, and it is a circuit question rather than a filter one.

| after pass 1 | compression, capture → chain | above 2 kHz, capture → chain | front lift it took |
|---|---|---|---:|
| `jcm800` | 16.1 → **16.0** | −16.4 → −11.8 | +4.0 dB |
| `bogner` | 18.2 → **18.4** | −10.8 → −9.5 | −1.1 dB |
| `slo` | 18.9 → **18.3** | −6.8 → −12.2 | +6.0 dB, at the limit |

`slo` is the one to look at next: it ran into the ±6 dB bound still 0.6 dB short on
compression, and its grain went the *wrong way* - the capture makes 5.4 dB more
above 2 kHz than the model does, and lifting the front made the gap wider, not
narrower. A fourth stage or a hotter third one is the kind of thing that closes
that; a filter is not.

### `harm` - the harmonic fingerprint, ours against the amplifier's

```bash
AG_MODEL=bogner build-host/tube_render harm "assets/audio/guitar-di/Bogner.nam"
```

A guitar take cannot separate what the distortion *made* from what a filter
*passed*: the take is a harmonic comb to begin with, so both land in the same
third-octave bands. Every voicing in this file was fitted through that ambiguity.

A sine burst has no such ambiguity - feed 82 Hz in and every decibel at 165, 247
and 330 was made by the chain - and a capture is a black box that will play
anything, so the same probe gives the real amplifier's ladder. The probe is six
notes (82 to 349 Hz) at three levels, then two-tone bursts, then a noise burst,
each with a guitar-like envelope; the analysis is a Goertzel at each harmonic, so
it is exact for the bin and costs one multiply per sample.

**Three things it had to get right, and two of them were wrong first.**

- **The same speaker on both sides.** Our side plays through a cabinet-carrying
  impulse; a head-only capture has no speaker at all, and against it every high
  harmonic on our side arrived 20 dB down - the capture read +4.1 dB at 6.3 kHz and
  ours −20.5. The noise burst at the end of the probe decides which case it is (the
  same rule `match` uses) and a head-only capture gets our cabinet put on it.
- **The same *filter* on both sides.** Our side then went through the *fitted
  impulse* while the capture side got the bare cabinet - two different filters. The
  correction's own bass lift showed up as intermodulation this chain does not make:
  the 49 Hz product moved 11 dB and the sign of the comparison reversed. Both sides
  now get `match_cab_<model>.wav` and nothing else.
- **A NAM capture is trained on guitar**, so a steady tone is outside what it has
  seen. Each note is run at three levels; if the ladder moves smoothly the model is
  interpolating, and if it jumps that row is not evidence. It does jump - the
  Bogner's H2 at 147 Hz reads −24.6, −44.4, −41.6 across levels - so rows are read
  as a group and not one at a time.

**What it says about the two models it can be run on** (both head-only captures, so
both comparisons are clean):

| | capture | ours | |
|---|---:|---:|---|
| `bogner`, H2 at 147 Hz | −44.4 dB | −24.6 dB | **20 dB more second harmonic** |
| `bogner`, H2 at 349 Hz | −22.4 dB | −4.3 dB | **18 dB more** |
| `bogner`, H3 | −20 to −30 dB | −19 to −31 dB | matched |
| `bogner`, 28 Hz from 82+110 | −31 to −40 dB | −24 to −25 dB | **7 to 15 dB more** |
| `slo`, H2 and H3 | | | within a few dB |
| `slo`, 49 Hz from 147+196 | −38 to −41 dB | −30 dB | **9 to 11 dB more** |

Two conclusions, and they are the same conclusion twice: **this chain is far more
asymmetric than the amplifiers it models, and it rectifies the envelope of a two
note chord far more.** Excess second harmonic and excess difference tones are what
one mechanism looks like from two directions - a stage that clips one side harder
than the other, and whose grid charge follows the envelope. Third harmonic, which
is what symmetric clipping makes, matches well.

That is a circuit finding rather than a filter finding, and it is where the
`jcm800` grain gap (4.6 dB too much above 2 kHz) and the `slo` fart both live. What
it does *not* point at is the coupling capacitors - see the next section.

### `cfg.couple_mul` - cutting the bass before each stage, and what that measures

Real high gain designs cut bass before each hot stage rather than a lot before the
first: the small interstage capacitors are what that is, 2.2 nF in a 2203. The
claim that goes with it is that the two arrangements are not the same amplifier
even when the overall response matches, because with one big cut the first stage
clips a signal with no bottom in it while with the cut distributed every stage
clips a signal that still has some.

Those corners come from the schematic through `ag_tube_couple_hz`, and until now
nothing could move them. `cfg.couple_mul[stage]` does - 1.0 is the schematic, 2.0
is half the capacitor - and it costs nothing at run time, because the coupling
high-pass is already a biquad per stage. It is also readable back as a component
value, which a bank of matching filters is not: a fitted 1.5 says *this amplifier
behaves as if that capacitor were 1.5 nF rather than 2.2*.

**And then the measurement said the opposite of what was expected.** With the
subsonic intermodulation of two low tones as the metric (`sens`, crunch model, and
positive means *more* of it):

| | compression | above 2 kHz | 49 Hz product |
|---|---:|---:|---:|
| stage 1 coupling ×2 | −0.0 dB | +0.0 dB | −0.0 dB |
| stage 2 coupling ×2 | −0.2 dB | +0.3 dB | **+1.3 dB** |
| stage 3 coupling ×2 | −0.2 dB | +0.2 dB | **+1.9 dB** |
| every stage ×1.26, the same total cut | −0.1 dB | +0.2 dB | **+4.2 dB** |
| +6 dB at 200 Hz *in front of everything* | +0.5 dB | −0.7 dB | **+8.1 dB** |

Cutting bass at the coupling caps makes the fart **worse** in this model, and
distributing the cut makes it worse than doing it at one stage. Stage 1 does
nothing at all, which is not a surprise: its corner is 1.6 Hz, so doubling it is
still 3.2 Hz.

**The first version of this knob was half a knob, and the measurement above is what
it produced.** It multiplied the coupling *high-pass corner* and nothing else - but
the same capacitor also sets the grid-charge time constant, `C × rgrid`, and
blocking is where most of this model's subsonic intermodulation is made (switching
it off takes **9.1 dB** off the 49 Hz product on the four-stage model, and the fourth
stage is where the rest of it appears). So the knob moved the audible corner and
left the nonlinear time constant at the schematic's value: a filter pretending to be
a component. It now divides `spec[i].ccouple`, next to the other component
overrides, and everything downstream - corner, blocking, bake - follows one value.

With that fixed the same experiment reads differently, and the size of the cut
matters: ×2 does nothing on `slo` (22 nF → 11 nF is still a 14 Hz corner), while
**×4 takes the 49 Hz product from −0.8 dB to −6.7 and ×8 to −7.9**. 22 nF → 2.8 nF is
an ordinary value in front of a hot stage, which is the whole point: the earlier
"cutting bass makes it worse" was measured with a knob that was not cutting bass
where it counted.

**And there is a second reason the corners are a weak lever, which fell out of the
host test written for them.** Doubling the second stage's 142 Hz corner costs the
80 Hz fundamental **0.4 dB**, and a first-order high-pass on its own would have
taken 5.5; eight times the corner costs 7.3 dB. The clipper puts the fundamental
back - past the knee the output's shape is set by the envelope and not by how much
of the fundamental arrived, which is the same reason pre-emphasis cannot push a top
end through a limiter. So a coupling cap in front of a hard-clipping stage does much
less to the *sound* than its corner frequency suggests, and much of what it does is
to the stages after it rather than to the one it feeds.

The lever that does work is the bass **level** in front of the valves: 8.1 dB of
intermodulation per 6 dB at 200 Hz. So in this model "cut the bass before the
clipper" lives in the pre-stage matching bank, not in the coupling capacitors -
and that is what pass one now uses. Why the corners behave that way is not settled;
the likeliest reason is that a first-order high-pass near the tones rotates their
relative phase, which changes the beat's crest factor and therefore how much of the
envelope gets rectified - the product is generated *after* the corner it was
supposed to be prevented by.

#### The rule these are chosen by: an octave below the lowest note

×8 on all three of `slo`'s hot stages was wrong, and the measurement that says so
is the second harmonic of a low E. The three do not share a grid leak, so the
schematic's corners are **7.1, 19.7 and 7.0 Hz**, and ×8 makes them 57, **158** and
56. The middle one is above the 82 Hz open E outright — and even 57 Hz is too high,
because a first-order high-pass there still costs an 82 Hz fundamental 2 dB while
costing its 164 Hz second harmonic a quarter of that, and three of them stack.

H2 on the low E, in dB under the fundamental, against a capture that reads
−37.7 / −17.4 / −28.8 at the three levels:

| | 82 Hz H2, quiet | mid | loud |
|---|---:|---:|---:|
| ×8, ×4, ×8 — 57, 79, 56 Hz | −6.8 | −7.4 | −7.1 |
| **×4, ×2, ×4 — 28, 39, 28 Hz** | −9.5 | **−20.0** | **−19.7** |
| ×1 — the schematic, 7, 20, 7 Hz | −7.3 | −10.5 | −9.6 |

A moderate cut beats both no cut and a big one, which is the domain rule this
started from; and the corner has to be an **octave** below the lowest note rather
than merely under it. Thirty decibels of asymmetry that no valve made, manufactured
by a high-pass filter and blamed on the valves for a week.

What it costs is the fart: at ×8 the 49 Hz product sat 2.7 to 7.5 dB above the
capture's, and at ×4/×2/×4 it is 8.7 dB above at full scale. That is the ladder's to
cut now, per stage, which is what a real design does — and `ladder` has it inside
its objective, one-sided, so a fit cannot buy anything by adding subsonic
intermodulation. Trading thirty decibels of false asymmetry on every low note for
six of a product on a double stop is not a close call.

`tube_render resp` prints every stage's corner and names any that break the rule,
and a host test asserts 41 Hz on all of them.

**Pass zero and pass one below are superseded** and are off by default: `ladder`
fits a block in front of every valve at the level where that valve starts working,
which is what those two passes were a single-parameter stand-in for. `AG_FIT_PRE=1`
brings them back, which is the right thing for a model that has never been through
the ladder. Their measurements are kept because they are how the 49 Hz product was
found in the first place.

### Pass zero: the bass cut in front of each clipping stage

The coupling capacitors get one bounded choice before any filter moves, because that
is what a real design does about this and because the metric for it now exists.
Four values are tried on the stages after the first - ×1, ×2, ×4, ×8, which on a
22 nF coupling is 22 down to 2.8 nF - and the first stage is left alone because it
measures as doing nothing at all (its corner is 1.6 Hz, so even ×8 is 13 Hz).

**The rule is not "minimise".** Cut only if this chain makes *more* of the 49 Hz
product than the amplifier does, and then only as far as it takes to reach the
amplifier's own figure. The first version of the rule did minimise, and on the
Marshall model that was a disaster: `jcm800` already puts the product 3 dB further
down than its own capture does, so there was nothing to fix, and ×8 was chosen
anyway - which took the bass out of a two-valve front end and dropped its
compression **from 12.2 dB to 7.0**, wrecking the one thing pass one is for. A cut
nobody asked for is not a reasonable cut.

What the rule does on the three models:

| | 49 Hz product, capture | ours at ×1 | pass zero picks | after pass zero | after pass one |
|---|---:|---:|---|---:|---:|
| `jcm800` | −26.8 dB | −30.0 dB | ×1, nothing to cut | −30.0 dB | −32.3 dB |
| `bogner` | −19.7 dB | −26.7 dB | ×1, nothing to cut | −26.7 dB | −27.6 dB |
| `slo` | −17.8 dB | **−0.8 dB** | ×8 → 2.8 nF | **−7.9 dB** | **−13.4 dB** |

`slo` was the model with the defect and it is the model the cut helps: **the product
went from as loud as the note to 13 dB under it**, and its compression landed on
18.9 dB against the capture's 18.9. Twelve and a half decibels of a real audible
fault, bought with one ordinary capacitor value and a mid lift. Four decibels of gap
are left, and by the user's own rule that is where this stops being a bass cut's
problem.

### Pass one, with three targets and two knobs

The character of the overdrive is three numbers now, and none of them can be moved
by a filter behind the valves - all three are measured with the post bank zeroed,
and the control row in `sens` confirms it reads 0.0 for all three:

1. **compression** - the same take 20 dB down, and how much came back;
2. **above 2 kHz** - how much of the output the valves made, where the DI has
   nothing;
3. **the 49 Hz product of 147 + 196 Hz** - the fart, which no single tone and no
   third-octave spectrum can see.

Pass one solves a 2×2 for two of them - a lift across 100 to 800 Hz against
compression, a cut at 100 and 200 against intermodulation - with the slopes
measured by `sens` and the step damped to a third, because the slopes are one
model's and the chain is not linear. What it settles on:

| | compression, capture → chain | 49 Hz product, capture → chain | what the front did |
|---|---|---|---|
| `jcm800` | 16.1 → **16.0** | −26.8 → −32.3 | 400 and 800 Hz up 6 dB, bass held |
| `bogner` | 18.2 → **18.2** | −19.7 → −27.6 | 400 and 800 Hz down 6 dB, bass held |
| `slo` | 18.9 → **18.4** | −17.8 → −0.7 | up 6 dB, at the bound |

The interesting column is the last one: both solutions move the *mids* in front and
leave the bass where it is, which is the loop discovering on its own that
compression is bought in the low mids and that buying it with bass costs
intermodulation.

And the `slo` row is the one to look at next. Its capture puts the 49 Hz product
17.8 dB under the tones; this chain puts it **0.7 dB** under them - the product is
as loud as the note. Nothing in front moves it (−0.8 to −0.7 across the whole
range), which says the mechanism is not level and not a filter. That is the
clearest single defect any of these measurements has found.

### `knee` - which valve is working, level by level

```bash
AG_MODEL=slo build-host/tube_render knee "assets/audio/guitar-di/5150red.nam"
```

In a cascade the last valve sees the biggest signal, so it distorts first. Start
quiet enough and only it is working; raise the level and the one before it joins,
and so on back to the first. So a level sweep separates stages that one loud take
mixes together, and each stage can be judged in the range where it is the only new
thing.

The separation is not guessed at: at each level the same 147 Hz note is rendered
through one stage, then two, then three, then four, and what a column adds is what
that stage contributed. A column that repeats the one before it is a valve doing
nothing yet. The amplifier's own column sits beside them from the same note at the
same level. Third harmonic is printed rather than second because H3 tracks how hard
a stage is working while H2 tracks how lopsided it is - that second question is what
`harm` is for.

On `slo`, third harmonic in dB below the fundamental:

| level | the capture | 1 stage | 2 stages | 3 stages | 4 stages |
|---:|---:|---:|---:|---:|---:|
| 0.02 | −17.8 | −64.8 | −54.1 | −53.6 | **−31.8** |
| 0.05 | −17.8 | −64.8 | −54.2 | −50.0 | −20.4 |
| 0.12 | −18.1 | −64.8 | −54.6 | **−28.6** | −20.4 |
| 0.28 | −18.4 | −64.8 | −57.4 | −29.8 | −23.4 |
| 0.55 | −18.2 | −64.7 | −53.9 | −29.8 | −27.0 |
| 0.95 | −18.5 | −64.3 | **−40.3** | −25.6 | −32.8 |

Three things fall out of it, and they are the clearest statement yet of what is
wrong with the four-stage model.

**The amplifier is level-independent and this chain is not.** The capture makes the
same relative third harmonic - −18 dB, within seven tenths - across a 34 dB range of
input. Ours moves by 12 dB and not even monotonically. Whatever a 5150 does, it does
it the same way whether you pick softly or hard, and that is most of what "it feels
like the amplifier" means.

**Two of our four valves are doing nothing.** One stage is at −64 dB at every level,
which is a linear amplifier; two stages reach −40 dB only at full scale. The
distortion is almost entirely the third and fourth stages' work.

**And the last stage is already saturated at the quietest probe.** At level 0.02 -
36 dB down, a soft pick - the four-stage column is at −31.8 dB while three stages are
at −53.6, so the whole difference is the last valve, and it is already well into it.
There is no regime in this model where the last valve distorts *gently*, which is
exactly the regime a level sweep was meant to isolate.

So the gain distribution across the four stages is wrong: too much of the signal
arrives at the end.

**The cheap fix was tried and a test rejected it.** Turning the last stage down -
`cfg.gain[3]` to 0.5, then 0.35 - does what the knee table asks: the fourth valve's
knee moves to between 0.05 and 0.12, so there is a quiet regime again, and the spread
across playing levels falls from 12 dB to 6. And `test_model_gain_order` fails at
both values, because the four-stage lead channel's small-signal gain drops **below
the three-stage crunch model's** - which is the exact failure that test was written
for, after a hand-picked divider once made this the cleanest of the three
amplifiers. A lead channel quieter than a crunch channel is not a lead channel,
whatever its harmonics look like.

Which means the work has to move to the *early* stages rather than away from the last
one, and inside "every knob at noon" that is the netlist - the plate loads and the
dividers in front of the cold clipper - not a gain constant. Reading that schematic
again is the open work; the knee table is the measurement to judge it by.

**One thing that did come out of the attempt:** `slo`'s `master` has followed the
chain a fifth time, 1/84 to 1/130. With the bass cut in and the banks refitted the
chain peaked at **+2.8 dBFS** on the fitting take, which showed up as an A/B render
clipped flat rather than as any number - `tube_render render` prints the master that
would have peaked at −1 dBFS, and that is where the value comes from each time.

### `ladder` - one tone block per valve, fitted where that valve works

```bash
AG_MODEL=slo build-host/tube_render ladder "assets/audio/guitar-di/5150red.nam" \
                                    build/listen/tube_di_22050.wav 2
```

**The procedure is the user's and it replaces pass zero and pass one above.** Start
with a signal small enough that nothing distorts and raise it until something does.
In a cascade the thing that goes first is the *last* valve, because it sees the
biggest signal - so at that level any difference in harmonic structure from the
capture belongs to the last valve, and the thing to adjust is the tone block in
front of it. Raise the level until the valve before it joins in; the new difference
is that valve's. Carry on back to the first. Then, if the chain as a whole
compresses by the wrong amount, the same trim on every stage. Then the output bank
and the impulse close the tone.

Two rules come with it and both are constraints on the code rather than remarks.
**The amplifier's own settings do not move** - `drive`, `g12`, `gain[]`, every
capacitor and resistor are the circuit, and the circuit is assumed right; what moves
is `cfg.voice[stage][]` and `cfg.vtrim[stage]`. And **absolute tone is not the target
at this stage**: what is being matched is what each valve is fed, and therefore what
kind of distortion comes out of it.

That needed a runtime change first. There was one matching bank in front of the
whole chain, which cannot express the thing a real high-gain design does - a little
bass out before *each* clipping valve rather than a lot before the first. With one
bank the fit has to choose, and what it chose is audible: the crunch model's single
bank came out at **+5.68 dB at 200 Hz in front of three clipping stages**, which is
the opposite of what the circuit wants. `cfg.voice` is now `[AG_AMP_STAGES][7]`, one
block per valve, each designed at the rate its stage runs at, and stage 0's is
bit-identical to what the single bank was.

#### What is measured, and the two corrections that took a second look

Five notes from low E to middle C at one level, and for each note the second through
sixth harmonic as decibels below its own fundamental - twenty-five numbers, which are
what "the same kind of distortion" means because dividing by the fundamental takes
the level out of it. Beside them the third-octave spectrum from 63 Hz to 5 kHz at
half weight, and the 49 Hz difference product of a 147 + 196 Hz double stop, also at
half weight.

**A plain H2-over-H1 taken at the output is not a measurement of distortion.** It is
distortion times whatever the linear network behind the valves does to those two
frequencies. A Marshall stack at noon has its dip at 400 Hz, so on a 349 Hz note it
takes eight decibels off the fundamental and almost nothing off the second harmonic,
and the ratio reads eight decibels of distortion that is not there - measured, H2 came
out at −3.4 dB on that note against the capture's −20.2, and most of seventeen
decibels was two tone stacks set differently. So each ratio has the device's own
small-signal response at both frequencies divided out of it, measured on the same
device by rendering a probe too quietly to distort.

That probe has to be a **chirp**, not the notes. The five-note probe has energy at
five frequencies and nowhere else, so a quiet render of it has nothing in the bands
where the harmonics land: the analyser reads its own floor and the correction comes
out as noise. The first version did exactly this and the capture's corrected
harmonic average came back at **+14 dB** - harmonics above the fundamental, which is
not a thing an amplifier does and was entirely the response of empty bands.

**And the 49 Hz product had to be inside the objective, not beside it.** A fit judged
on twenty-five harmonic ratios drove the four-valve chain deep into blocking
distortion and reported that the character had improved: harmonics within a decibel
of the capture, and the 49 Hz product **12 dB above it**, which is the sound of a low
chord farting. No single note shows that, which is the whole reason the term exists.
It is one-sided - less subsonic intermodulation than the amplifier is not a fault
anybody has complained about, and a two-sided term would let the search *add* it to
score a point.

#### Which level belongs to which stage is measured

The chain is taken apart the way `knee` takes it apart - one stage, then two, then
three - and stage k's rung is the quietest one where adding it puts more than 3 dB of
harmonics on top of what the stages in front were already making. Recomputed every
pass, because fitting a block moves the knee behind it.

Three things about that turned out to matter.

**The first stage has nothing to add over.** Compared against a floor of −300 dB it
"adds" 250 dB at every level, so it landed on the bottom rung every time - the one
place it is provably not working. For that one the rule is absolute instead: the
quietest level at which one valve alone makes −40 dB of harmonics.

**A stage that never adds 3 dB has no rung and must not be given one.** The
four-valve model's last stage is a recovery stage: across the whole ladder it adds
nothing - at the bottom rung it comes out half a decibel *quieter* than three stages
alone. Handing it the top rung as a default and then applying "never below the stage
behind it" dragged all four up to full scale, so every stage was fitted on one rung
with the guard rungs collapsed into it. What came back was **+30 dB of matching gain
and ten decibels too much distortion.** It is now left alone and excluded from the
ordering.

**And a stage is not judged on its own rung alone.** The block being fitted is in the
signal path at every level, and a version that scored only the stage's own rung found
answers that were better there and catastrophic above - the three-valve chain came
back with harmonics **above** the fundamental at half scale, +7 dB, having been
improved by two decibels at a two-hundredth of it. The score is the stage's own rung
at double weight and three more spread to full scale.

#### The two things that are not per-stage

**The tilt.** The ladder fixes what each valve is fed; what it cannot reach is how the
drive is *shared*, and on both high-gain models that is the biggest single thing
wrong. `knee` says one valve does all the clipping and the ones in front of it are
linear amplifiers, and two audible differences come straight out of that. The chain
makes 8 dB more harmonic content than the capture once both are saturated - and at
saturation the amount no longer depends on level, so no drive change fixes it; what
fixes it is fewer stages clipping as hard. And the second harmonic is 15 to 25 dB
high while the third is within two, which is asymmetry - and asymmetry is what a
cascade of *inverting* stages cancels, each valve flattening the half its predecessor
left alone. Several valves sharing the work come out more symmetric than one valve
doing all of it. Ours has one valve doing all of it, so nothing cancels.

So the drive is tilted from the back of the chain to the front by one parameter,
**with the sum held at zero**. Sum-zero is the whole difference between this and the
`gain[3]` attenuator that was tried and reverted: that one cut the last stage and
gave nothing back, so the model's small-signal gain fell below the model below it and
`test_model_gain_order` caught it. A tilt takes the same decibels off the back and
puts them on the front - the gain the chain has is the gain it had, earned in a
different place.

**The compression trim, which may decline.** Bisection assumes the thing it is
solving for is monotone *and reachable*. Compression is monotone in drive but on an
already-saturated chain it is very nearly flat: the four-valve model was 1.2 dB short
and the bisection answered "+8.9 dB on every stage", thirty-six decibels of gain
bought to move one - and it landed on the bound, which is a search reporting that it
could not do the job while sounding as if it had. It is now a bounded scan that
picks the smallest trim that helps, and does nothing at all if nothing inside ±6 dB
is a third of a decibel better than nothing.

And the per-stage trims are brought back to **zero mean** before it runs. Left alone
they summed to +30 dB on the four-valve model - thirty decibels of gain manufactured
in the matching layer, which is not a matching correction but the statement that the
circuit's gain is wrong, and that belongs in the netlist. The trims redistribute; the
compression step is the only thing that changes the total, and it has a measurement
behind it.

Each round of the band search also ends by taking the bank's mean out of the bank and
into the trim. Seven peaking sections and a flat trim are redundant, and a search
handed redundant parameters will use them: the two-valve model came back with −7.5 dB
at 3.15 and 5 kHz in front of the first valve and +7.5 dB at the same two in front of
the second - a level shift written as fourteen decibels of tone that cancels, with
four bands pinned at their bound and no shape left to fit.

#### What it produced, and what it cost

Every number below is with the ladder's banks and trims in, the output bank refitted
on top of them, and the master re-read. Compression is the 20 dB down test against
each model's own capture; the spectral figures are mode 1, the bank plus an impulse
fitted to the remainder.

| | compression, capture → chain | spectrum, fitting take | spectrum, **30.9 s held out** |
|---|---|---:|---:|
| `jcm800` | 16.1 → **16.3** | 0.70 → **0.29** | 3.51 → **1.18** |
| `bogner` | 18.2 → **18.0** | 0.54 → **0.27** | 1.97 → **0.73** |
| `slo` | 18.9 → **18.9** | 0.50 → 0.62 | 2.33 → **2.33** |

The held-out column is the one that means anything, and it is the column that moved:
the two-valve model generalises three times better than it did. `slo` is a wash
there and slightly worse on its own take, which is the trade the procedure asks for
— it optimises what the valves make and lets the impulse close the tone.

What actually changed in character, which is what the exercise was for — and what
did not, which matters more:

| | before | after |
|---|---|---|
| `slo` mean harmonics across 34 dB of input | swings 12 dB | **flat, −18.7 against the capture's −18.2** |
| `slo` H3 at 147 Hz | 20 dB off | within 1 dB |
| `slo` H2 from 110 to 262 Hz | 10–20 dB high | within 1 to 6 dB |
| `bogner` mean harmonics, saturated | 8.7 dB too many, rising with level | 4.4 dB, level-independent |
| **`slo` H2 on the low E** | −7 dB (capture −37.7) | **−9.5 dB — still the worst number in the model** |
| **`slo` 49 Hz product** | 12.4 dB above the capture | **10.9 dB** |
| **`bogner` H2** | 15–25 dB high | **10–18 dB high** |

The top four are what the procedure was for and it delivered them. The bottom three
are the ones a listener complained about and they have barely moved, so they are the
work, not the result.

**And one of them is a flaw in this tool rather than in the chain.** The 49 Hz term
inside the objective is divided by each device's own linear response, like the
harmonic ratios — and for that one it should not be. A fart is an absolute level at
the output: what the ear gets is the product against the notes, however the tone
network arrived at it. Corrected, the term reads 2.6 to 4.6 dB and the fit believes
it has closed the gap; uncorrected, `harm` measures **10.9 dB**. Nothing else catches
it either, because the output bank and the impulse are fitted over third-octave bands
from 63 Hz up and 49 Hz is below the first of them. So the pipeline has a hole
exactly where the loudest complaint lives, and closing it means an uncorrected IMD
term plus a band under 63 Hz in the output fit.

The `jcm800` row that is missing from that table is worth a sentence of its own: our
third harmonic is **15 to 20 dB below** its capture's at every note. That capture is
a hot-rodded Marshall with its gain on 8; this chain runs at `drive` 0.5, which the
house rule calls noon. Matching a gain-8 capture from a noon chain asks the matching
layer to supply the difference, and asking is what the +5.5 dB of compression trim on
every stage is. Either the reference should be a noon capture or this model's drive
is not 0.5 — that is a decision about what the model *is*, not a fit to run.

The bill is real and it is on the chip. `tubebench bench` under `-icount`, at the
4× with ADAA that ships:

| | before | after | with the cabinet |
|---|---:|---:|---:|
| `jcm800`, two valves | 32.7% of a core | **45.3%** | 54% |
| `slo`, four valves | — | **77.1%** | 86% |

The per-stage banks are about 24 points of `slo`'s figure: twenty-one sections at four
times the sample rate, at 31 instructions each. Seven of its twenty-eight bands sit
under 1 dB and twelve under 1.5, so a threshold in `design()` would buy back 6 to 10
points for a change nobody can hear — that is the obvious lever if the chip needs it,
and it has not been pulled because it is a judgement about audibility rather than a
measurement.

### `irnoise` - which post-valve mode the chip's arithmetic prefers

```bash
AG_MODEL=bogner build-host/tube_render irnoise build/listen/tube_di_22050.wav
```

Mode 1 and mode 2 are equally accurate in exact arithmetic, so the choice between
them comes down to what `ag_ir` does with each. Each mode's own render is convolved
with its own impulse twice - once exactly and once through the engine the chip runs
- and the difference is the cost. Levels are matched by least squares and the best
lag is searched for first, because neither a gain nor a delay is an error (the lag
comes out 0, but that had to be established: differencing them straight would have
blamed one sample of latency on the arithmetic).

| error against an exact convolution | mode 1, bank + impulse | mode 2, impulse alone |
|---|---:|---:|
| `jcm800` | **−54.2 dB** | −46.5 dB |
| `bogner` | **−26.1 dB** | −22.2 dB |
| `slo` | **−24.5 dB** | −22.0 dB |

**Mode 1 is cleaner on every model, by 2.5 to 7.7 dB**, which is the hypothesis
confirmed: an int16 impulse asked to carry a 19 dB tilt spends its range on the
tilt, and mode 1 leaves that tilt in seven exact biquads. So the recommendation
that came out of the tone measurements - mode 2, on the grounds of architecture and
fewer parts - is overturned by the arithmetic: **ship mode 1**.

The absolute numbers are worth their own line. −22 to −26 dB against an exact
convolution is not a rounding difference, and it is not the impulses either: the
engine reproduces every one of them to 0.00 dB in every third-octave band on a
delta. It is what the tree already records about `ag_ir` on decaying material -
error to signal −66 dB at full scale, **−13 dB in the note decays** - integrated
over a take with long tails. `tube_live`'s float path hides it; the chip has it.

### `sens` - what a filter in front of the valves actually does

```bash
AG_MODEL=bogner build-host/tube_render sens build/listen/tube_di_22050.wav
```

One band at a time, +6 dB, and what came out. This is the measurement that
constrains the fit above, and it answers a question that had only ever been
answered by assertion - *raising the bass in front of a clipper makes a lot of
harmonics, or does it?*

| +6 dB in front at | what it does to the spectrum | compression | made above 2 kHz |
|---|---|---:|---:|
| 100 Hz | +2 dB in 80-160, nothing above 500 | +0.1 dB | **−0.3 dB** |
| 200 Hz | +2 dB in 125-250, −0.4 to −0.7 above 1.2 kHz | **+0.5 dB** | **−0.7 dB** |
| 400 Hz | everything down 1-4 dB (1 kHz rises most) | +0.4 dB | −0.0 dB |
| 800 Hz | bass and low mids down 1-3 dB | +0.1 dB | +0.4 dB |
| 1600 Hz | +0.6 to +1.8 from 1.2 kHz up, bass −1 | +0.1 dB | **+0.9 dB** |
| 3150 Hz | +1.3 to +2.0 from 2 kHz up, bass untouched | −0.0 dB | **+0.9 dB** |
| 5000 Hz | +1.2 to +1.5 at 4-6.3 kHz | −0.0 dB | +0.4 dB |
| **200 Hz, behind the valves** (the control) | +5.6 dB at 200, −0.3 up top | **+0.0 dB** | **+0.0 dB** |

Read from the bottom up. The control row is what "a filter" means: it moves the
spectrum by what a filter moves and changes neither character number by a tenth of
a decibel. Every row above it is the same 6 dB in front, and they do different
things:

- **Bass in front does not make harmonics** - it makes *fewer*, in relative terms
  (−0.3 dB at 100 Hz, −0.7 at 200), because it raises the fundamental the harmonics
  are measured against and pushes the whole chain further into compression.
- **The low mids are the drive control.** 200 and 400 Hz move compression more than
  anything else (+0.5, +0.4) and are what pass one uses.
- **400 Hz is the clearest mechanism in the table**: +6 dB there drops every band by
  1-4 dB *relative to 1 kHz*, which is to say it raises 1 kHz - because 400 Hz's own
  second and third harmonics land at 800 and 1200. A band in front fills the octave
  above itself.
- **Grain comes from presence in front, not from bass.** 1.6 and 3.15 kHz add the
  most above 2 kHz (+0.9 dB each) while leaving compression alone: they do not drive
  the valves harder, they give the clipper something up there to multiply.
- **5 kHz in front is nearly inert** (+0.4 dB of grain, +1.2 dB of its own band),
  which is the same finding as the old one recorded above - a limiter cannot be
  pre-emphasised into supplying a top end the guitar does not have.

No cabinet in any of it: this is about the valves, and a loudspeaker only takes the
top away again.

## Authoring a preset for another amplifier

Everything below is already true of the JCM800 in the tree; this is the order to
do it in for a second one, because two of these steps invalidate the ones after
them and finding that out by hand is expensive.

**1. The reference material, and where it comes from.**  Both files under
`build/listen` are build artefacts with no rule that rebuilds them, so the recipe
belongs here:

- `tube_di_22k.wav` is `assets/audio/guitar-di/GuitarClean2.wav` (mono, 16-bit,
  7.931 s), and **the name no longer describes it**: the copy in the tree today is
  byte-for-byte the source at 44.1 kHz, not a 22.05 kHz decimation.  Both forms
  have been in that path, so check the rate before trusting a measurement made
  against it - `tube_live` opens the sound card at whatever rate this file has,
  which is how a buffer queue sized in blocks came to hold half the audio it was
  designed for.  Either form is one command:

  ```bash
  build-host/wavrate assets/audio/guitar-di/GuitarClean2.wav tube_di_22k.wav 22050
  ```

  The 22.05 kHz form is right when `render ... 0.1 4 1 4` with `AG_TOP_HZ=1000`
  prints a cabinet trim of +10.9 dB and `humps` reads +13.6 dB.
- `ref_mars_22k.wav` is the same take through a NAM capture of the reference
  amplifier (`assets/audio/guitar-di/Mars Gain 8.nam`), resampled to 22050 Hz.
  It has to come from the same DI take, because the fit and the hump metric both
  assume the two files are sample-aligned.

  **This tree now plays .nam files itself** - `tools/nam.c`, with `nam_run` as a
  front end and `match` doing it in memory:

  ```bash
  build-host/nam_run "assets/audio/guitar-di/Mars Gain 8.nam" \
                     build/nam/GuitarClean2_48k24.wav out.wav
  ```

  It was a patched fork of [waveny](https://github.com/mikeoliphant/waveny) in Go,
  outside the repository, and that was the single most losable thing in the whole
  exercise: a disk cleanup would have taken the ability to check any tone match
  ever again.  The port is a straight one - same loop order, same float
  accumulators - because the test on it is exactness rather than similarity:

  | rendered through | against | result |
  |---|---|---|
  | `Mars Gain 8.nam`, GuitarClean2 | the stored `_mars` render | **bit identical** |
  | `Bogner.nam`, GuitarClean2 | the stored `_bogner` render | **bit identical** |
  | `Mars Gain 8.nam`, E2_s1_01 (31 s) | the stored `_mars` render | **bit identical** |

  The last one matters because it is long enough to rewind the model's 65536-column
  history buffers twenty times, which is where an off-by-one would show.
  `host-tests/test_nam.c` keeps the first of the three as a permanent check, over
  the first three blocks so it costs a tenth of a second, and skips itself when the
  captures are absent - they are third-party files and are not in the repository.

  Four things had to be added to upstream waveny and are in this port too:
  LeakyReLU, per-layer kernel sizes, a convolutional head, and the TONE3000
  container.  Every one of them is checked by the parameter count - the
  architecture implies exactly how many numbers it needs and the file says how
  many it has - because a model that loads with the wrong topology does not fail,
  it confidently plays something that is not the amplifier it claims to be.  Every
  other mechanism in the schema (FiLM conditioning, grouped convolutions, gating,
  secondary activations) is refused rather than ignored, for the same reason.
  `docs/08-circuit-simulation.md`, section "Сверка с чужой моделью", is the Russian
  original of this note.

  The captures live beside the DI takes: `Mars Gain 8.nam` (the JCM800 this chain
  is voiced against), `Clean.nam`, `SLAMMIN_VOX_AC30_N_V8_TC8_S.nam`, `Bogner.nam`,
  `5150red.nam`, `Ibanez.nam`, `VX TB30 BR Edge0 BAL2 CAB FREE.nam`.

  **Where the Go fork is, for anybody who wants to check this port against it.**
  `D:\dev\waveny`, copied out of the session scratchpad under `%TEMP%` it used to
  live in - it is not in the repository and it is not on any other machine, and
  while nothing depends on it any more, it is the only independent opinion
  available about what these files sound like.

  ```bash
  D:/dev/waveny/namrun.exe -model "assets/audio/guitar-di/Bogner.nam" \
             -input build/nam/GuitarClean2_48k24.wav \
             -output build/nam/GuitarClean2_bogner.wav
  ```

  **The pipeline, and the naming that records it.**  NAM captures run at 48 kHz
  and everything here at 44.1, so `tools/wavrate.c` sits on both ends of the
  render - polyphase sinc, Kaiser window beta 12, 64 taps a phase, 10241 for
  44.1 <-> 48; a 44.1 -> 48 -> 44.1 round trip differs from its source by -64 dB,
  all of it at the 20.7 kHz cutoff, so it takes no part in any argument about the
  top end.  Under `build/nam`, `<take>_48k24.wav` is the input side (48 kHz,
  24-bit) and `<take>_<model>.wav` the output, the suffix naming the capture -
  `_mars`, `_nam` for Clean, `_vox`, `_bogner`, `_5150red`.  Frame counts match
  in and out, because the model is sample-for-sample.  `imp.wav` and
  `imp_mars.wav` are half a second each: an impulse in, the model's response out,
  which is where the cabinet impulses came from.

  `match` does all of that in memory instead, in one process, so the reference it
  fits against has no 16-bit file anywhere in it - see the next section.

  **If `ref_mars_22k.wav` is missing**, the 48 kHz render it came from is
  `build/nam/GuitarClean2_mars.wav`, and

  ```bash
  build-host/wavrate build/nam/GuitarClean2_mars.wav ref_mars_22k.wav 22050
  ```

  reproduces it; a regenerated copy scored 1.27 dB rms against the fitted
  voicing where the original had recorded 1.29, which is as close as two
  resamplings of the same render get.

  **What to look for in a capture, since this is the step that gates everything
  after it.** Four things matter, in this order:

  1. **The same DI take, rendered by you.** A capture someone else has already
     printed to audio is useless here: the fit compares band by band on
     sample-aligned files, so the reference has to be *this* take through *that*
     model. What is wanted is the model file, not a demo of it.
  2. **Amp only, no cabinet.** Captures come both ways and the labels vary -
     "no cab", "IR-less", "DI", "amp only", "FX return". A capture with a cabinet
     baked in fits fine, but then the cabinet is part of the reference and
     `AG_CAB_IR` has to be the same speaker or step 2 above is being fought
     rather than followed.
  3. **The channel and the gain setting in the name.** `bogner` is aimed at a
     crunch channel and `slo` at a saturated lead channel; a capture of the
     same amplifier's clean channel is a different amplifier for this purpose.
     Captures usually carry the knob positions in the file name, and the fit is
     run at one drive setting - so a capture at "gain 5" and a model at drive 0.5
     are comparable in a way that "gain 10" is not.
  4. **A single 12AX7-preamp architecture, not a channel-switching monster.**
     What this chain can be is two or three cascaded 12AX7 stages with a tone
     stack; a capture whose amplifier has a fourth stage, a cathode follower or a
     tube-driven effects loop is a target the topology cannot reach, and the fit
     will spend its bands trying.

  What the two new models were fitted against, and it satisfies all four:
  `Bogner.nam` is "Bogner Ecstasy - Bright Crunchy Rock" - an Ecstasy 101B,
  `tone_type` crunch - and `5150red.nam` is "5150 RED GRINDR", a Peavey 5150,
  `tone_type` metal. Both came from `tone3000.com`, which is where captures live
  now (it absorbed ToneHunt).

  **Whether a capture has a speaker in it is a measurement, not a label.**  Both of
  those say `gear_type: "amp"` in their metadata, and the metadata is worth exactly
  as much as the person who filled it in; the top two octaves settle it. A 4×12 is
  20 to 40 dB down up there and an amplifier on its own is not:

  | render, dB re 1.3-2.5 kHz | 5.1-10 kHz | 10-20 kHz |
  |---|---:|---:|
  | the dry DI take | −15.8 | −63.0 |
  | `Mars Gain 8`, `gear_type: amp_cab` | −24.1 | **−42.2** |
  | `Bogner` | −3.4 | **−8.8** |
  | `5150red` | −4.9 | **−6.7** |

  So neither of the new two has a cabinet, and they cannot be listened to or fitted
  as they stand - a preamp with no speaker is 30 dB of fizz above 5 kHz. Give them
  ours:

  ```bash
  build-host/wavrate build/nam/GuitarClean2_bogner.wav ref_bogner_22k.wav 22050
  build-host/tube_render cabify ref_bogner_22k.wav ref_bogner_22k_cab.wav
  ```

  `cabify` exists for exactly this and nothing else: a file in, the same file
  through `AG_CAB_IR`, out. **This is the better position of the two**, and it is
  why the crunch model fits to 0.86 dB where the Marshall reached 1.27: with the
  speaker absent from the capture, the same impulse goes on both sides of the
  comparison and the choice of it cancels. A capture with a 4×12 baked in forces
  the other side to be an impulse extracted from that same capture, which is an
  approximation nobody measured.

**2. The cabinet, before anything else.**  It decides more than any control: the
low end moved 11 dB on the impulse alone, and a voicing fitted through the wrong
one only reaches 2.53 dB rms where the right one reaches 1.29.  Point `AG_CAB_IR`
at the impulse that belongs with the amplifier being modelled and leave it there.
Changing it later means re-running step 4.

**3. The spec, from the schematic.**  Copy the shape of `ag_tube_spec_jcm800` in
`ag_tube.c`: one `ag_tube_spec_t` per stage, every field documented at the struct
in `ag_tube.h`.  Keep the discipline that function already keeps - the same
component values also live in `ckt_jcm800_stage` in `apps/cktbench/ckt_circuits.c`
and `test_tube.c` checks them field by field, because two copies of a resistor
value drift silently and then get blamed on the valve model.  `cfg.n_stages` is
1 to 4; a stage with no table is a clean makeup stage, and the first stage's
filter block runs at the base rate while every later one runs oversampled.

`ag_tube_spec_bogner` and `ag_tube_spec_slo` are the two worked examples, and
the second one is the more useful: it has three stages, an unbypassed cathode, and
a comment per departure saying what the departure buys.  Then add the id to
`ag_amp_model_id`, a block to `ag_amp_model` for everything a spec cannot carry -
stage count, the gain into each stage, the two voicing knobs, `master` - and a
line to `ag_amp_spec`.  Everything else follows: `AG_MODEL=` in `tube_render`,
`--model` in `tube_live`, the third argument of `tubebench`, and the id travelling
inside every preset the model bakes.

**Say whether it has been fitted.**  `ag_amp_model_fitted` is the one line that
keeps the difference honest, because an unfitted chain does not sound broken - it
sounds like a working amplifier that is not the one on the label.  Until step 4
has been run against a capture, the answer is 0 and every tool prints it.

**4. Fit the voicing.**

```bash
AG_MODEL=bogner build-host/tube_render fit build/listen/ref_bogner_22k_cab.wav \
                                           build/listen/di_gc2_22k.wav 40 0.5
```

`di_gc2_22k.wav` rather than `tube_di_22k.wav` on purpose: that path has held the
take at two different rates (step 1), and a fit run against a reference at 22.05 kHz
with a DI at 44.1 compares nothing. The two files a fit is given have to be the same
take at the same rate, and it is worth keeping a copy whose name says which.

It moves each third-octave band by a fraction of its own error and repeats, and it
prints its answer as the block to paste into `ag_amp_model`.  Read
"Matching the spectrum of a reference" above before believing the result: which
bank a band lands in is physics, not bookkeeping - harmonics have to be **made**
before they can be shaped, a limiter cannot be pre-emphasised into supplying a
top end, and more bass in front of a clipping valve is less midrange out of it.

Run it at zero iterations first.  That scores the voicing the model already
carries without moving it, which is the measurement that says whether the chain is
short of the reference by a decibel or by fifteen - and it is how the dead voicing
banks above were found.

Two things to know if the model sets `tone_shelf`, as both new ones do.  The fit
moves each band by the error at its own centre, and a first-order high shelf is
only half of its own dB there, so the numbers it converges to are about twice what
a peaking section would have needed - that is arithmetic, not a fault.  And the
shelves are the reason to prefer it anyway: a peaking section at +15 dB rings for
1.4 ms behind a clipper, which is the 3.9 kHz rattle this file spends eight
experiments on.

**And it overshoots, so read the iteration number.**  Both new fits got their best
answer early and then walked away from it: the crunch model reached 0.86 dB rms at
iteration 8 and was back at 1.56 by 40, the three-valve one 1.71 dB at iteration 11
against 3.46 by 40.  The tool keeps the best rather than the last and prints which
it was; forty iterations are not forty improvements, they are how far it has to walk
to prove the minimum was a minimum.

**5. The master, after the fit and not before.**  Fifteen decibels of fitted
presence is fifteen decibels of peak; `render` prints the master that would have
peaked at -1 dBFS on that take, and the right value moves with the drive and the
recording.

**6. Bake and save.**

```bash
build-host/tube_render preset build/listen/bogner.preset 2.0 bogner
```

The model is the third argument and it wins over `AG_MODEL`, because a preset is a
file that will still be there next month and a blob named after one amplifier
holding another is the one mistake worth designing against.  The three in the tree
come to 49 864 bytes for a two-valve chain and 74 440 for three.

The second argument is the drive the axes are fitted at, and **2.0 is measured,
not chosen**: one times the top of the knob is 33 dB worse than a fresh bake
because the probe drives the second grid less than a real guitar does, and four
times is worse again because the resolution it loses costs more than the headroom
it buys.  Loading needs no solver, no Newton and no axis fitting, so an
application that only plays presets never links `ag_ckt`; the tables carry no
sample rate, so one preset plays at 22.05 and 48 kHz alike.

**What not to spend time on.**  The 3.9 kHz rattle is not a defect to fix per
amplifier - the sections above record eight attempts on it, all measured, all
either ineffective or rejected, and the mechanism (every harmonic above 2 kHz is
born in the same 0.3 ms of the clipping edge, with 4.5 ms of silence between
edges) says why the whole family fails.  It is a property of a two-stage static
curve, it is 0.5 dB from a real amplifier's own figure at the default top cut,
and it is louder to a listener than the metric says it is.

## Reproducing

```bash
argon tests
```

```bash
build-host/tube_render curve
```

`curve` prints the baked curves and operating points, `resp` the three filters,
`alias` what oversampling is worth, `imd` the two-tone products, `res` how many
points and how much axis, `noise` whose hiss it is, `floor` the peak and noise
floor of any file with the floor split by band, `cab` what the cabinet impulse
does, and

```bash
build-host/tube_render render in.wav out.wav 0.5 4 1 4 1.0
```

runs a take through it — `drive os adaa cab g12` — fitting the axes to that take
rather than to the synthetic probe, because a probe has neither the spectrum nor
the dynamics of a guitar. It writes `out.wav` dry and `out_cab.wav` through the
cabinet. **Drive 1.0 is the top of the range and 0.5 the working setting**; above
1.0 nothing new happens except compression and noise. Point `AG_CAB_IR` at your
own impulse to replace the Vox AC30.

Every mode takes `AG_MODEL=jcm800|bogner|slo` and prints which model it ran and
whether that model's voicing has ever been fitted:

```bash
AG_MODEL=slo build-host/tube_render curve
```

prints all three valves including the cold one, and `render` reports what each
stage's grid actually saw against the axis it was baked over — which is the line to
read when a stage sounds like a fuzz. `AG_G23=` moves the divider in front of the
third stage, the way `g12` moves the one in front of the second.

On the chip, in instructions rather than host nanoseconds:

```bash
argon apps --only TUBEBENCH.AXE
```

and then `run h:\tubebench.axe bench h:\tube.txt slo` under
`argon test -HostFs build\sd_card`: the model is the third argument, because a
third valve inside the oversampled region is not a number that can be got by
multiplying the two-valve one.

```bash
build-host/tube_live --model bogner
```

plays it in a loop with the knobs live — `h` prints the map, ESC quits. The model
is fixed for the run and starts at its own stage count; it is not a key because
this tool bakes one chain per stage count at startup so that nothing ever stalls
mid-note, and three models would be three times that.
