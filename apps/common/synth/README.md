# ag_synth — VA / N-op FM + modulation matrix

Userspace fixed-point synth on [`ag_dsp`](../dsp). No `libm`.

- VA: 2 osc (PolyBLEP saw/square, calculated tri/noise, LUT sine, custom WT from WAV) → SVF → FET/tube dist → amp ADSR
- Custom osc: `ag_osc_set_table` / `ag_synth_set_wavetable` (caller loads WAV via `ag_wav`)
- FM: 2..8 operators, free route matrix (`ag_fmx`)
- `ag_synth_mod_bind(src, dest, depth)` — LFO/EG/vel/note/MW/AT → any param
- VA: osc2 can PM osc1 (`FM_INDEX`); LFO → `OSC2_TUNE` / `FM_INDEX` modulates the modulator
- FM: 3-op stack, `FM_INDEX` = last link, `FM_INDEX2` = mod-of-mod

Routing depth in `ag_fmx` is a **phase offset in 1/65536 of a cycle**, shifted
into the 32-bit operator phase before use — at depth 64 a full-scale modulator
bends the carrier by about 1.2 radians. Adding a raw sample to the phase
instead moves it by a millionth of a cycle and produces no sideband at all,
which is what the engine used to do.

Demo: [`apps/synth`](../../synth) (`SYNTH.AXE`).

Listen: `build-host/synth_smoke.exe` writes `build/synth_smoke.wav`;
`build-host/engine_smoke.exe` writes `build/listen/*.wav` for every engine.
Both are built by `argon tests` — they are not tests, but they must not stop
compiling. What *is* asserted lives in
[`host-tests/test_dsp.c`](../../../host-tests/test_dsp.c).

Apache-2.0.
