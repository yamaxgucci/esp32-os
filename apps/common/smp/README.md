# ag_smp — ROM-плеер / сэмплер

Один буфер (зона): высота по MIDI, линейная интерполяция, loop, ADSR, 8 голосов.

Это не `ag_grain` (облако зёрен) и не мультисэмпл/SoundFont. Свой WAV
кладёт вызывающий (`ag_wav` + `ag_smp_set_zone`). Встроенные пресеты —
синтетические (орган с петлёй, пианино, бас), чужих сэмплов в дереве нет.

Огибающая — общая `ag_dsp_adsr_t`, тикается **на каждый сэмпл**, поэтому
`ag_dsp_adsr_set_rate` вызывается в `ag_smp_note_on`. См.
[`ag_dsp`](../dsp/README.md).

Host: `build-host/engine_smoke.exe` пишет `build/listen/smp_*.wav`
(собирается вместе с host-тестами).
