# MOUSEVIRT — virtual mouse from Windows

Loadable `.SYS` that publishes **`/dev/mouse0`** and injects
`AG_EV_POINTER_*` / `AG_EV_WHEEL` into the console input queue (ABI 0.18
`inp->inject`). Guest listens on TCP **`:5560`**; host
[`tools/mousevirt.py`](../../tools/mousevirt.py) connects.

No client → silent; apps use `ag_poll_event` as usual.

Canonical paths (mkaxe stages both):

- `build/apps/MOUSEVIRT.SYS`
- `build/sd_card/MOUSEVIRT.SYS`

## Build

```
python tools/mkaxe.py --arch xtensa --gcc xtensa-esp32s3-elf-gcc `
  --include sdk/include -o build\apps\MOUSEVIRT.SYS apps/mousevirt/mousevirt.c
```

## Install once

```
drv install h:\mousevirt.sys
```

Persists via `[modules]` in `C:\SYSTEM.CFG` (same as PCMVIRT / MIDIVIRT).

## Host mouse → guest

`.\argon.cmd run` hostfwd’s **5560** (with 5558/5559).

```powershell
$py = "D:\Espressif\tools\python_env\idf5.5_py3.12_env\Scripts\python.exe"
& $py tools\mousevirt.py --reconnect
```

Maps the primary monitor to the guest framebuffer (default 640×400).
**Right-Ctrl** pauses sending; **Esc** in the mousevirt console quits.

## Packet (8 bytes LE)

| offset | field |
|--------|--------|
| 0 | type (`1` abs, `3` wheel) |
| 1 | buttons (bit0=L, bit1=R, bit2=M) |
| 2..3 | x int16 |
| 4..5 | y int16 |
| 6 | wheel int8 |
| 7 | pad |

Optional: apps may also `read()` `/dev/mouse0` for the same packed events.
