# ag_dsp — shared fixed-point audio primitives

Userspace math for ArgonOS synth/FX engines. No `libm`, no device I/O.

`ag_sat16`, `ag_clampi`, sine LUT, MIDI note → Hz, phase step, delay tap,
voice steal, linear ADSR, LFO waves, xorshift RNG.

Used by `ag_dx7`, `ag_grain`, `ag_fx`, `ag_ir`, `ag_smp`, `ag_synth`. Link
`apps/common/dsp/ag_dsp.c` and `--include apps/common/dsp`.

## Two contracts worth reading before you call this

**The envelope counts milliseconds, not ticks.** Tell it how often you will
tick it, then tick it:

```c
ag_dsp_adsr_set_rate(&e, rate);          /* once, at note-on or init */
ag_dsp_adsr_on(&e);
ag_dsp_adsr_tick(&e, a, d, s, r, gate);          /* per sample      */
ag_dsp_adsr_tick_n(&e, a, d, s, r, gate, frames) /* or per block    */
```

A/D/R are 0..127 and map quadratically to time — attack 1..3000 ms, decay
2..4000 ms, release 3..5000 ms — so the same knob means the same thing to
`ag_synth` (ticking every sample) and `ag_grain` (ticking once per render
block, whatever the block size). It did not always: a per-tick increment made
the longest possible attack 1.3 ms in the synths and 325 ms in grain, and
every note was a click.

**Voice stealing wants the note-on sequence number.** Stamp `born[i] = ++seq`
and `ag_dsp_voice_steal` takes the *lowest* one — the oldest note. Passing
something that counts the other way round steals the note just played.

Covered by [`host-tests/test_dsp.c`](../../../host-tests/test_dsp.c); it runs
in `argon tests`.

Apache-2.0.
