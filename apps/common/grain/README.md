# ag_grain — fixed-point granular engine

Userspace Clouds-shaped grain cloud: position, size, density, spray, pitch,
texture (box→tri→Hann), pan spray, reverse chance, per-note ADSR, freeze buffer.

- Output: stereo s16 @ caller rate (typically 22050)
- Buffer: mono s16 (WAV load or future mic append via `ag_grain_buf_append`)
- Viz: `ag_grain_viz_update` fills active grain dots for UI

No libm. See [`apps/grain`](../../grain).
