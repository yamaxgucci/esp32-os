# SMS — Master System player for ArgonOS

Mute port of **SMS Plus GX** (GPLv2+) as an ArgonOS `.AXE`. Video via soft
`gfx`; sound disabled; keyboard maps to two pads.

## License

- `core/` and `LICENSE` — SMS Plus GX, **GPLv2 or later**
- `sms_main.c`, `sms_cfg.*`, `port/` (except where noted) — Apache-2.0

The kernel stays Apache-2.0; this application image is a separate GPL work.

## Build

```
python tools/mkaxe.py --arch xtensa --gcc xtensa-esp32s3-elf-gcc `
  --include sdk/include --include apps/sms/port --include apps/sms/core `
  --include apps/sms/core/z80 --include apps/sms/core/sound `
  --cflags "-Os -ffunction-sections -fdata-sections -DLSB_FIRST -DNOZIP_SUPPORT -DUSE_Z80 -Wno-unused -Wno-sign-compare" `
  -o build/SMS.AXE `
  apps/sms/sms_main.c apps/sms/sms_cfg.c `
  apps/sms/port/libc_shim.c apps/sms/port/platform.c `
  apps/sms/core/loadrom.c apps/sms/core/memz80.c apps/sms/core/pio.c `
  apps/sms/core/render.c apps/sms/core/sms.c apps/sms/core/system.c `
  apps/sms/core/tms.c apps/sms/core/vdp.c `
  apps/sms/core/z80/z80.c `
  apps/sms/core/sound/sound.c apps/sms/core/sound/fmintf.c `
  apps/sms/core/sound/ym2413.c apps/sms/core/sound/sn76489.c
```

`-DLSB_FIRST` is required (Xtensa is little-endian; the Z80 core's `PAIR` union
depends on it).

## Run in QEMU

```
argon run -Gfx -HostFs build\sms_share
```

```
run h:\sms.axe h:\game.sms
```

With `-HostFs` (and `sms.cfg`), **hostfsd pushes** live pad state ~60 Hz over
UART1.  Guest caches it; `H:\sms.pad` reads that cache — **no per-frame RPC**,
so diagonals / move+fire work without killing FPS.  Look for
`sms: live pad H:\sms.pad (host push)`.

Focus can be on the **SDL window** (Win32 hook) or the terminal.  Keys come
from the host keyboard sampler, not from UART autorepeat.

Without HostFS / without a pad push, SMS falls back to **serial sticky keys**
(time-based TTL).  Force that with `nolivepad`.

### What is `H:\sms.pad`?

A **virtual 3-byte file** (pad0, pad1, sys).  On the host, `hostfsd --pad-cfg`
samples the Windows keyboard and **pushes** snapshots (`HSFS_OP_PADPUSH`).
The guest drain task updates a RAM cache; opening/reading `H:\sms.pad` is local.

Old pull-every-frame mode was too slow on QEMU UART1 and is gone from the
play path.

## Controls (defaults)

Guest always uses these bindings for serial sticky keys (no on-disk `sms.cfg`
in-guest — that path could hang). Host `sms.cfg` only matters for `livepad`.

```
pad0.up=UP
pad0.down=DOWN
pad0.left=LEFT
pad0.right=RIGHT
pad0.b1=Z
pad0.b2=X
pad0.pause=ENTER
pad0.quit=ESC

pad1.up=W
pad1.down=S
pad1.left=A
pad1.right=D
pad1.b1=J
pad1.b2=K
pad1.pause=P
pad1.quit=Q
```

| Default | Pad 0 | Pad 1 |
|---------|-------|-------|
| D-pad | Arrows | WASD |
| B1 / B2 | Z / X | J / K |
| Pause | Enter | P |
| Quit | Esc | Q |

## Headless smoke

```
argon test -Put build\SMS.AXE=t:\sms.axe -TimeoutSec 180 `
  "run t:\sms.axe 180" "errorlevel" "gfxdump t:\sms.ppm"
```

Args: `run sms.axe rom.sms` (forever), `run sms.axe rom.sms 300` (N frames),
`run sms.axe 60` (tiny cart, N frames), `run sms.axe rom.sms nolivepad`
(force serial sticky).
