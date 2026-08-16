# DX7 — structural 6-op FM synth

Polyphonic (8 voices) DX7-shaped FM app (not a ROM emulator): 6 operators,
32 algorithms, rate/level EG, feedback, pitch EG, LFO. Structural lite — not
bit-exact / SysEx-complete. Engine: [`apps/common/dx7`](../common/dx7).

`apps/fm` is the **file manager** — unrelated.

## Build

Normally just `argon apps` (or `argon apps --only DX7.AXE`) — the
authoritative build line for this image lives in [`tools/apps.json`](../../tools/apps.json)
and is compiled by CI. The command below is the same thing, spelled out:

```
python tools/mkaxe.py --arch xtensa --gcc xtensa-esp32s3-elf-gcc `
  --include sdk/include --include apps/common --include apps/common/dsp `
  --include apps/common/pcm --include apps/common/dx7 `
  --include apps/common/fx --include apps/common/libc `
  --cflags "-Os -ffunction-sections -fdata-sections -fno-builtin" `
  -o build/apps/DX7.AXE `
  apps/dx7/dx7.c apps/common/dsp/ag_dsp.c apps/common/pcm/ag_pcm.c `
  apps/common/dx7/ag_dx7.c apps/common/dx7/ag_mid.c `
  apps/common/fx/ag_fx.c apps/common/libc/libc_shim.c
```

`mkaxe` stages into `build/apps/` and `build/sd_card/` (see
[`docs/user/03-host-share.md`](../../docs/user/03-host-share.md)).

## Realtime on Windows

By default the app writes to **`/dev/pcmnull`**. For host speakers: install
[`PCMVIRT.SYS`](../pcmvirt) once (`drv install h:\pcmvirt.sys`), then pass
`pcmvirt` (or `net`). Full checklist: [`apps/pcmvirt/README.md`](../pcmvirt/README.md).

PowerShell / Windows Terminal (repo root):

```powershell
$py = "D:\Espressif\tools\python_env\idf5.5_py3.12_env\Scripts\python.exe"
```

Terminal 1:

```powershell
.\argon.cmd run -Gfx -HostFs build\sd_card
```

Guest:

```
drv install h:\pcmvirt.sys
drv install h:\midivirt.sys
run h:\dx7.axe pcmvirt
```

Terminal 2 (audio):

```powershell
& $py tools\pcmplay.py --reconnect
```

Terminal 3 (poly keyboard → guest MIDI-in):

```powershell
& $py tools\midikbd.py --reconnect
```

Focus the midikbd window. Same piano map as the guest (`Z..M` / `Q..I`).
See [`apps/midivirt/README.md`](../midivirt/README.md).

Optional: `& $py tools\pcmplay.py --record build\dx7.wav`.

| Arg | Effect |
|-----|--------|
| *(default)* / `mock` / `mute` / `pcmnull` | render → `/dev/pcmnull` |
| `pcmvirt` / `net` / `tcp` | `/dev/pcmvirt` → `pcmplay.py` |
| `audio` / `i2s` / `pcm0` | `/dev/pcm0` (future I2S `.SYS`) |
| `midivirt` / `midi` | open `/dev/midivirt` (default: try) |
| `nomidi` | skip MIDI-in device |
| `nofx` | dry output (default; no FX buffers until first enable) |
| `fx0` | same-core: render → delay/chorus/reverb → write |
| `fx1` | FX worker on `AG_THREAD_SYS_CORE` (+1 chunk latency) |
| `path.syx` | load DX7 SysEx bank (1 or 32 voices) |
| `path.mid` | load Standard MIDI file (loops) |

FX library: [`apps/common/fx`](../common/fx) (~25 KB heap when enabled; app
arena 192 KB).

Demo bank / MIDI on the HostFS share:

```
python tools/dx7syx.py gen -o build/sd_card/dx7/ROM.SYX
python tools/dx7mid.py -o build/sd_card/dx7/DEMO.MID
```

On start the app auto-loads the first `.syx` under `h:` / `h:\dx7`.
A `.mid` file is loaded and looped only if you pass its path on the command
line (keyboard / `midikbd.py` otherwise).

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
| `F` | Cycle FX mode: off → local → core1 → off |
| `D` / `C` / `V` | Toggle delay / chorus / reverb (while FX ≠ off; steals those piano keys) |
| `8` / `9` | Master wet −/+ (while FX ≠ off) |
| F1–F6 / F7–F12 | Op level −10 / +10 |
| Esc | Quit |

Plain serial consoles have no KEY_UP → mono (each key releases the previous).
With win32-input / kitty key events (`CONFIG_ARGON_CONSOLE_KEY_EVENTS`) → poly chords.

UI shows `Keys : held … last … src midivirt+kbd`, plus `Perf` /
`Stream` (chunk ~20 ms @ 22050 Hz).

## Future (backlog)

- **Per-op / LFO waves:** keep sine (`isin`) and current LFO shapes; add
  `wave_id` so each operator and the LFO can select sine / built-in shapes /
  a **custom one-cycle table loaded from `.wav`** (alternative, not a global
  replace of `isin`). See [`docs/06-ideas.md`](../../docs/06-ideas.md).
- More polyphony / multi-timbral, bit-exact EG — deferred.
- Guest nofx structural synth on Argon CC: [`apps/cc/examples/dx7nofx.c`](../cc/examples/dx7nofx.c)
  (phase E). This host `DX7.AXE` remains the FX / WAV / richer UI line.
