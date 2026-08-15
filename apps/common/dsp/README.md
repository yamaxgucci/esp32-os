# ag_dsp — shared fixed-point audio primitives

Userspace math for ArgonOS synth/FX engines. No `libm`, no device I/O.

`ag_sat16`, `ag_clampi`, sine LUT, MIDI note → Hz, phase step, delay tap,
voice steal, linear ADSR, LFO waves, xorshift RNG.

Used by `ag_dx7`, `ag_grain`, `ag_fx`, `ag_ir`, `ag_synth`. Link
`apps/common/dsp/ag_dsp.c` and `--include apps/common/dsp`.

Apache-2.0.
