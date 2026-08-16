# ag_mp3 — streaming MP3 for ArgonOS apps

Thin wrapper around vendored [minimp3](../../../third_party/minimp3) (CC0).

- `ag_mp3_open` / `ag_mp3_read` / `ag_mp3_seek_permille` / `ag_mp3_close`
- Output: interleaved s16 stereo (mono is duplicated)
- Used by [`apps/amp`](../../amp)
