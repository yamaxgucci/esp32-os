# ag_synth — VA / N-op FM + modulation matrix

Userspace fixed-point synth on [`ag_dsp`](../dsp). No `libm`.

- VA: 2 osc (PolyBLEP saw/square, sine, tri, noise) → SVF → FET/tube dist → amp ADSR
- FM: 2..8 operators, free route matrix (`ag_fmx`)
- `ag_synth_mod_bind(src, dest, depth)` — LFO/EG/vel/note/MW/AT → any param

Demo: [`apps/synth`](../../synth) (`SYNTH.AXE`). Host listen file:
`tools/synth_smoke.c` → `build/synth_smoke.wav`.

Apache-2.0.
