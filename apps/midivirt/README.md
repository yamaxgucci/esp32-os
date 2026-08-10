# MIDIVIRT — virtual MIDI-in from Windows

Loadable `.SYS` that publishes **`/dev/midivirt`**. The guest listens on TCP
**`:5559`**; the host (`tools/midikbd.py`) connects and sends raw MIDI note
on/off. Apps non-blocking `read()` packed 4-byte events:

| offset | field |
|--------|--------|
| 0 | status (`0x9n` / `0x8n` / `0xBn`) |
| 1 | note or CC |
| 2 | velocity / value |
| 3 | pad (0) |

No client → `read` returns 0; the app never waits.

Canonical paths (mkaxe stages both):

- `build/apps/MIDIVIRT.SYS`
- `build/sd_card/MIDIVIRT.SYS`

## Build

```
python tools/mkaxe.py --arch xtensa --gcc xtensa-esp32s3-elf-gcc `
  --include sdk/include -o build\apps\MIDIVIRT.SYS apps/midivirt/midivirt.c
```

## Install once

```
drv install h:\midivirt.sys
```

Persists via `[modules]` in `C:\SYSTEM.CFG` (same as PCMVIRT).

## Host keyboard → guest

`.\argon.cmd run` hostfwd’s **5558** (pcmplay) and **5559** (midikbd).

```powershell
$py = "D:\Espressif\tools\python_env\idf5.5_py3.12_env\Scripts\python.exe"
& $py tools\midikbd.py --reconnect
```

Piano map matches DX7 (`Z..M` / `Q..I`). Esc quits; Space = all notes off.
Focus the midikbd console so Win32 sees the keys.

## With DX7 + sound

Three terminals (repo root):

```powershell
.\argon.cmd run -Gfx -HostFs build\sd_card
# guest:
#   drv install h:\pcmvirt.sys
#   drv install h:\midivirt.sys
#   run h:\dx7.axe pcmvirt

& $py tools\pcmplay.py --reconnect
& $py tools\midikbd.py --reconnect
```

DX7 UI line `Keys : held … src midivirt+kbd` shows pressed notes.
