# ag_ckt — nonlinear analogue circuit solver

Modified nodal analysis with Newton-Raphson, one audio sample per solve. Not a
waveshaper: a netlist of resistors, capacitors, sources, diodes, JFETs and
triodes is assembled into a matrix, capacitors are replaced by their
trapezoidal companion model at the sample rate, and every sample is solved
until the node voltages stop moving. What SPICE does, with the timestep nailed
to 1/fs.

| File | What |
|---|---|
| `ag_ckt.h` / `ag_ckt.c` | netlist, stamping, device models, LU, Newton |
| `ag_stage.h` / `ag_stage.c` | the same circuit with a fixed topology, baked |
| `ag_mathf.h` / `ag_mathf.c` | `expf`/`logf`/`powf`/`sqrtf`/`tanhf` without libm |

**`ag_ckt` is the reference; `ag_stage` is what ships.** Solving the circuit at
audio rate costs 16 572 instructions a sample for one valve — 331% of an
ESP32-S3 core at 48 kHz. Baking the same circuit costs **385**, and the two
agree to between −63 and −104 dB. The bake is not a simplification: the table
holds the answer this solver gives, computed once instead of eighty thousand
times a second.

With antialiasing a stage is 625, and with a whole tone stack folded into the
same matrix, 925. Three valves and one tone stack, twice oversampled, come to
17.4 ms of processor time per 20 ms of audio — 87% of one core.

`ag_mathf` exists because `apps/common/libc` returns **zero** from `expf`,
`logf` and `powf` — those are link stubs, not functions. A device equation
built on them gives a valve that never conducts and a signal that stays clean,
which is a failure that looks like a working clean channel.

## Devices

| Model | Cost per Newton pass |
|---|---|
| Antiparallel diode pair (Shockley) | one `exp`, one divide |
| N-JFET (Shichman-Hodges) | no transcendentals at all |
| Triode (Koren, 12AX7 defaults) | one `sqrt`, one `pow`, two or three `exp` |

The triode includes grid conduction as a soft knee, which is what makes a valve
stage compress and shift its bias when hit hard. Without it the stage clips
symmetrically and sounds like a fuzz box.

## Using it

```c
ag_ckt_t k;
ag_triode_model_t m;
ag_triode_model_12ax7(&m);

ag_ckt_init(&k, 48000.0f);
ag_ckt_add_vin(&k, 1, 0);                 /* driven by ag_ckt_tick   */
ag_ckt_add_vdc(&k, 2, 0, 250.0f);         /* B+                      */
ag_ckt_add_c(&k, 1, 3, 22.0e-9f);         /* coupling                */
ag_ckt_add_r(&k, 3, 4, 470.0e3f);         /* grid stopper            */
ag_ckt_add_r(&k, 4, 0, 100.0e3f);
ag_ckt_add_r(&k, 2, 5, 100.0e3f);         /* plate load              */
ag_ckt_add_r(&k, 6, 0, 1500.0f);          /* cathode                 */
ag_ckt_add_c(&k, 6, 0, 22.0e-6f);         /* bypass                  */
ag_ckt_add_triode(&k, 4, 5, 6, &m);
ag_ckt_build(&k);

float out = ag_ckt_tick(&k, vin, 5);      /* node 5 is the plate     */
```

Node 0 is ground. `ag_ckt_build` factors the matrix once if the circuit has no
nonlinear device, so a filter or a tone stack costs two triangular solves per
sample and no iteration.

## Baking one

```c
ag_stage_init(&stage);                    /* once, before the first bake  */
ag_stage_bake(&stage, &k, 5, table, integral, 256, 9,
              p1_lo, p1_hi, p2_lo, p2_hi);
ag_stage_set_adaa(&stage, 1);             /* needs the second buffer      */
float out = ag_stage_tick(&stage, vin);   /* no exp, no matrix, no Newton */
```

`integral` may be `NULL` if antialiasing is not wanted; with it, the stage
returns the average of the device currents over the sample instead of their
value at the instant, which is what removes the aliasing an overdrive makes.
It costs a second table of the same size and half a sample of delay.

**Do not rewrite the averaging as the definition has it.** `(H(b) - H(a)) /
(b - a)` is the same formula and it crackles: twice a cycle the denominator
passes through zero while the numerator is a difference of nearly equal
cumulative integrals, and the quotient is noise of constant size - loudest,
relative to the music, as a note decays. Inside one cell the table is a
straight line and the mean of a straight line is its midpoint value, so both
the subtraction and the division cancel algebraically; across a few cells the
pieces are added up one cell at a time. The cumulative table is only touched
for jumps of more than four cells, where the step is large enough to divide by
safely. Measured: −45 dB of noise the naive way, −202 dB this way, for the same
cost. `test_ckt.c` has a quiet-signal check that fails on the naive form.

The ranges are the whole art of it, and `ckt_bake_chain` in
[`apps/cktbench/ckt_circuits.c`](../../cktbench/ckt_circuits.c) shows the
procedure that works: bake wide, settle to the operating point, run the signal
that will actually be played and watch where the driving point goes, bake
tightly around that, settle again. Getting that order wrong - measuring the
range before the bias has settled, or against a steady tone rather than a
plucked note - leaves the table too narrow, and `stage.clamped` counts every
sample the model then spends guessing.

## Two things that are not obvious and cost real time

**Cascades want a matrix each, not one big matrix.** Stages multiply, so the
solver's own rounding at the first grid comes out of the last plate multiplied
by the gain of the whole chain; at about 25× per stage single precision is
spent by the third valve. Solve stage by stage and pass a voltage across —
which is all the topology allows anyway, since the stages are coupled through
a capacitor into a high-impedance grid. It is also five times cheaper, because
the solve is cubic in the number of unknowns.

**The matrix needs equilibrating, not just row scaling.** A 22 µF cathode
bypass at 48 kHz stamps 2.1 S next to a 470 kΩ grid stopper's 2.1 µS, and the
columns are as lopsided as the rows. Scaling rows and columns by the same
diagonal — in exact powers of two, so the scaling itself rounds nothing — is
what makes Newton converge in three passes instead of sitting in a limit cycle.

## Checked by

* [`host-tests/test_ckt.c`](../../../host-tests/test_ckt.c) — `ag_mathf`
  against libm, a linear RC against the closed form (including bilinear
  warping), the triode Jacobian against finite differences, the 12AX7 operating
  point, gain and clipping asymmetry, the JFET stage, and the cascade.
* [`build-host/ckt_smoke`](../../../tools/ckt_smoke.c) — renders
  `build/listen/ckt_*.wav`, including the pair that measures what oversampling
  is worth.
* [`apps/cktbench`](../../cktbench) — what it costs, in instructions, on the
  guest.

Numbers and the analysis: [`docs/08-circuit-simulation.md`](../../../docs/08-circuit-simulation.md).

Apache-2.0.
