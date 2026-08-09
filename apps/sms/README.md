# SMS — Master System player for ArgonOS

Port of **SMS Plus GX** (GPLv2+) as an ArgonOS `.AXE`. Video via soft `gfx`.
Sound is **opt-in** (PSG + FM → WAV or mock; no I2S yet).

FM is **not** the heavy `ym2413.c` emulator. `fmintf` decodes the YM2413/OPLL
register protocol and drives [`apps/common/fm`](../common/fm) (`ag_fm`) — a
lightweight integer synth (own timbre, same note/volume/key protocol).

## License

- `core/` and `LICENSE` — SMS Plus GX, **GPLv2 or later**
- `sms_main.c`, `sms_cfg.*`, `port/`, `apps/common/fm` — Apache-2.0

The kernel stays Apache-2.0; this application image is a separate GPL work.

## Build

Do **not** link `ym2413.c`. Include `sound_wav.c` and `ag_fm.c`.

```
python tools/mkaxe.py --arch xtensa --gcc xtensa-esp32s3-elf-gcc `
  --include sdk/include --include apps/common/libc --include apps/sms/port `
  --include apps/sms/core --include apps/sms/core/z80 `
  --include apps/sms/core/sound --include apps/common/fm `
  --cflags "-Os -ffunction-sections -fdata-sections -fno-builtin -DLSB_FIRST -DNOZIP_SUPPORT -DUSE_Z80 -Wno-unused -Wno-sign-compare" `
  -o build/SMS.AXE `
  apps/sms/sms_main.c apps/sms/sms_cfg.c `
  apps/common/libc/libc_shim.c apps/sms/port/platform.c apps/sms/port/sound_wav.c `
  apps/common/fm/ag_fm.c `
  apps/sms/core/loadrom.c apps/sms/core/memz80.c apps/sms/core/pio.c `
  apps/sms/core/render.c apps/sms/core/sms.c apps/sms/core/system.c `
  apps/sms/core/tms.c apps/sms/core/vdp.c `
  apps/sms/core/z80/z80.c `
  apps/sms/core/sound/sound.c apps/sms/core/sound/fmintf.c `
  apps/sms/core/sound/sn76489.c
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

Default play is **mute** (best FPS). Enable sound:

| Arg | Effect |
|-----|--------|
| `mock` or `sound` | PSG + FM (`ag_fm`), samples discarded |
| `wav` | write `t:\sms.wav` |
| `t:\out.wav` (any `*.wav`) | write that path — **must** end in `.wav` or it is ignored / treated as ROM |

## Speed and measurement

| Arg | Effect |
|-----|--------|
| `stats` | print fps, per-frame work split (emu / show), max, and **% realtime** every 2 s and at exit |
| `fps30` | emulate all 60 frames, rasterise and show every second one |
| `fps60` | show every frame (default) |

`% realtime` is against one NTSC frame of 16667 µs, so 100% means exactly keeping
up. In QEMU it is only a lower bound of sanity — QEMU models neither the cache
nor PSRAM latency, so it cannot prove realtime on a board, only rule it out.

The emulator renders **straight into the `gfx` back buffer** and flushes only its
own rectangle; there is no intermediate frame and no blit. Budgets and the plan
behind this: [`docs/07-emulator-performance.md`](../../docs/07-emulator-performance.md).

Prefer `T:` or `A:` for capture (HostFS every frame is slow). `T:` is ~4 MB
RAM (PSRAM); long free-run captures can still fill it. Then in the **same**
session (reboot clears `T:`):

```
copy t:\sms.wav h:\capture.wav
del h:\old.wav
```

Look for `sms: sound = wav t:\sms.wav @ 22050 Hz` and `sms: wav closed, …`.


```
run t:\sms.axe 60 nolivepad mock
run t:\sms.axe 60 nolivepad wav
dir t:\sms.wav
```

With `-HostFs` (and `sms.cfg`), **hostfsd pushes** live pad state ~60 Hz over
UART1.  Guest caches it; `H:\sms.pad` reads that cache — **no per-frame RPC**.

Without HostFS, SMS falls back to **serial sticky keys**. Force with `nolivepad`.

## Controls (defaults)

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

## Headless smoke

```
argon test -Sd -TimeoutSec 120 `
  "run a:\SMS.AXE 30 nolivepad mock" "errorlevel" `
  "run a:\SMS.AXE 60 nolivepad wav" "errorlevel"
```

Speed:

```
argon test -Sd -TimeoutSec 240 `
  "run a:\SMS.AXE 600 nolivepad stats" "errorlevel" `
  "run a:\SMS.AXE 300 nolivepad fps30 stats" "errorlevel"
```
