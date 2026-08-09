# ag_fm — lightweight integer FM for ArgonOS apps

Userspace synth (not the kernel `audio` / I2S class). No float, no `libm`.

SMS maps the YM2413/OPLL register protocol onto this API in
`apps/sms/core/sound/fmintf.c`. Mega Drive can reuse the same core later with a
different register front-end (YM2612).

## API

See `ag_fm.h`: `ag_fm_init`, `ag_fm_reset`, `ag_fm_set_fnum`, `ag_fm_set_key`,
`ag_fm_set_inst_vol`, `ag_fm_set_patch`, `ag_fm_update`.

Apache-2.0.
