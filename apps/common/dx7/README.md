# ag_dx7 — structural DX7-like 6-op FM

Userspace fixed-point synth (not the kernel `audio` class, not `ag_fm`).

- 6 operators, 32 algorithms, feedback on OP1
- Per-op rate/level EG (4+4), pitch EG, LFO (PMD/AMD)
- 8-voice polyphony with steal
- Integer sin LUT, no `libm`

Used by [`apps/dx7`](../../dx7). Separate from OPLL/OPN [`apps/common/fm`](../fm).

Apache-2.0.
