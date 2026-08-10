# DX7 — structural 6-op FM synth

Polyphonic (8 voices) DX7-shaped FM app (not a ROM emulator): 6 operators,
32 algorithms, rate/level EG, feedback, pitch EG, LFO. Structural lite — not
bit-exact / SysEx-complete. Engine: [`apps/common/dx7`](../common/dx7).

`apps/fm` is the **file manager** — unrelated.

## Build

```
python tools/mkaxe.py --arch xtensa --gcc xtensa-esp32s3-elf-gcc `
  --include sdk/include --include apps/common/dx7 --include apps/common/libc `
  --cflags "-Os -ffunction-sections -fdata-sections -fno-builtin" `
  -o build/DX7.AXE `
  apps/dx7/dx7.c apps/common/dx7/ag_dx7.c apps/common/libc/libc_shim.c
```

Copy `DX7.AXE` to a HostFS share (e.g. `build\sms_share` or `build\cc_share`).

## Realtime on Windows (same path as MD)

By default the app is **mute** (`mock`). Pass **`net`** to stream stereo s16
WAV PCM @ 22050 Hz over TCP `:5558` → host `tools/pcmplay.py`.
`argon run` hostfwd’s `127.0.0.1:5558`.

Terminal 1:

```
argon run -HostFs build\cc_share
```

Guest:

```
run h:\dx7.axe net
```

Terminal 2 — after the guest prints `waiting for host`:

```
python tools/pcmplay.py
```

Optional: `pcmplay.py --record build\dx7.wav`, or `net:PORT` + `argon run -NetPort PORT`.

| Arg | Effect |
|-----|--------|
| *(default)* / `mock` / `mute` | render, discard |
| `net` / `tcp` | TCP → `pcmplay.py` |
| `net:PORT` | same, custom port |
| `audio` / `i2s` | kernel `api->audio` (I2S or stub) |
| `path.syx` | load DX7 SysEx bank (1 or 32 voices) |
| `path.mid` | load Standard MIDI file (loops) |

Demo bank / MIDI on the HostFS share:

```
python tools/dx7syx.py gen -o build/cc_share/dx7/ROM.SYX
python tools/dx7mid.py -o build/cc_share/dx7/DEMO.MID
```

On start the app auto-loads the first `.syx` and `.mid` under `h:` / `h:\dx7`.
MIDI drives the synth in a loop (keyboard optional).

## Keys

| Keys | Action |
|------|--------|
| Z–M | C4–B4 (до–си); Q–I upper octave |
| `[` `]` | Algorithm −/+ |
| `-` `=` | Feedback −/+ |
| `,` `.` | Patch −/+ (bank voice if .syx loaded) |
| `K` `L` | Bank voice −/+ |
| `Tab` | Next `.syx` on HostFS |
| `;` | Back to builtin presets |
| `1`…`6` / `0` | Op on/off / all on |
| `'` | Mute carriers |
| ← → / ↑ ↓ | Mod wheel / aftertouch |
| `P` / `O` | Portamento / unison |
| Enter / `\` | MIDI play-stop / restart |
| Space | Panic (all notes off) |
| F1–F6 / F7–F12 | Op level −10 / +10 |
| Esc | Quit |

Plain serial consoles have no KEY_UP → mono (each key releases the previous).
With win32-input / kitty key events (`CONFIG_ARGON_CONSOLE_KEY_EVENTS`) → poly chords.

UI shows `Perf` (render/send/loop µs, % of chunk budget) and `Stream`
(late / dropped bytes / resync). Chunk is ~20 ms @ 22050 Hz.
