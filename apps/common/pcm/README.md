# ag_pcm — PCM sink helper

Open `pcmvirt` / `pcmmix` / `pcmnull` (fallback to mute), write interleaved s16,
wall-clock pace, `GETSTATS`. A `*.wav` argument writes RIFF on VFS/HostFS.

```c
ag_pcm_t pcm;
ag_pcm_open(&pcm, argv_sink, 22050, 2);
ag_pcm_pace_start(&pcm);
ag_pcm_write(&pcm, stereo, (int32_t)pcm.chunk);
ag_pcm_pace_wait(&pcm);
ag_pcm_close(&pcm);
```

Link `apps/common/pcm/ag_pcm.c` with `--include apps/common --include apps/common/pcm`.

Apache-2.0.
