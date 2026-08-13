# KBDVIRT — virtual keyboard from Windows

Loadable `.SYS` that publishes **`/dev/kbd0`**. The guest listens on TCP
**`:5561`**; the host (`tools/kbdvirt.py`) connects and sends 8-byte key
packets. `read()` pumps the socket and injects `AG_EV_KEY_DOWN` / `UP`
(same idea as MOUSEVIRT → pointer events).

| offset | field |
|--------|--------|
| 0 | type: 1=down, 2=up |
| 1 | mods (`AG_MOD_*`) |
| 2–3 | HID usage id (uint16 LE) |
| 4–5 | unicode (uint16 LE, often 0) |
| 6 | repeat |
| 7 | pad |

No client → `read` returns 0; the app never waits.

Canonical paths (mkaxe stages both):

- `build/apps/KBDVIRT.SYS`
- `build/sd_card/KBDVIRT.SYS`

## Build

```
python tools/mkaxe.py --arch xtensa --gcc xtensa-esp32s3-elf-gcc `
  --include sdk/include -o build\apps\KBDVIRT.SYS apps/kbdvirt/kbdvirt.c
```

`python apps/doom/build.py` also builds this and bakes `build/sdcard.img`.

## Install once

```
drv install a:\kbdvirt.sys
```

Persists via `[modules]` in `C:\SYSTEM.CFG`.

## Host → guest

`argon run` hostfwd’s **5561**. SDL is video-only; keys for Doom come from
the host tool (not HostFS PADPUSH).

```powershell
.\argon.cmd run -Gfx -Sd
# guest, once:  drv install a:\kbdvirt.sys
# guest:        run a:\doom.axe -iwad a:\doom1.wad

python tools/kbdvirt.py --reconnect
```

Right-Ctrl pauses injection (so you can type at the serial console).
Ctrl+C quits the host tool. Arrows / Ctrl / Shift / Space / Enter / Esc
match vanilla Doom.
