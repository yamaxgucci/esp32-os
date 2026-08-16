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

Normally just `argon apps` (or `argon apps --only MOUSEVIRT.SYS`) — the
authoritative build line for this image lives in [`tools/apps.json`](../../tools/apps.json)
and is compiled by CI. The command below is the same thing, spelled out:

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
.\argon.cmd run -Gfx -Virt
# or, if QEMU is already up:
python tools/virt.py
# standalone:
python tools/mousevirt.py --reconnect
```

Maps the letterboxed QEMU client (default guest 640×400).
**Right-Ctrl** pauses sending; **Esc** quits only when the mousevirt console
is focused (`virt.py` has no console — Ctrl+C / QEMU exit stops it).

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
