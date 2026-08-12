# AMP — Winamp-style MP3 player

Soft-gfx player: streaming MP3 (minimp3), 10-band EQ, playlist, classic
bitmap skins for **640×400 (VGA)** and **320×240 (QVGA)**. Mouse and full
keyboard control.

Decoder: [`apps/common/mp3`](../common/mp3) — streaming minimp3 (VFS/HostFS or
SD on hardware). Playback is wall-clock paced so fast decode cannot overrun
`pcmvirt`. Skins: [`skins/vga`](skins/vga), [`skins/qvga`](skins/qvga) —
regenerate / stage with `python tools/skin2rgb565.py`.

## Build

```
python tools/skin2rgb565.py

python tools/mkaxe.py --arch xtensa --gcc xtensa-esp32s3-elf-gcc `
  --include sdk/include --include apps/common --include apps/common/mp3 `
  --include apps/common/libc --include third_party/minimp3 `
  --cflags "-Os -ffunction-sections -fdata-sections -fno-builtin -DMINIMP3_NO_SIMD" `
  -o build/apps/AMP.AXE `
  apps/amp/amp.c apps/amp/amp_cmds.c apps/amp/amp_ui.c apps/amp/amp_input.c `
  apps/amp/amp_skin.c apps/amp/amp_eq.c apps/amp/amp_playlist.c `
  apps/common/mp3/ag_mp3.c apps/common/libc/libc_shim.c
```

Stages to `build/apps/` and `build/sd_card/`. Put MP3 files under
`build/sd_card/amp/` (or `music/`).

## Run (QEMU)

QEMU + HostFS is much slower than a real ESP32-S3: soft gfx blit, HostFS
reads, and float EQ all compete with decode. Underruns sound like crackle and
make the on-screen timer lag realtime. Prefer `pcmnull` to measure decode-only,
or hardware + SD/`pcm0` for actual listening. Flat EQ (default) skips the
biquad chain.

```powershell
.\argon.cmd run -Gfx -HostFs build\sd_card
# guest:
#   drv install h:\pcmvirt.sys
#   drv install h:\mousevirt.sys
#   run h:\amp.axe pcmvirt
```

Host helpers (mouse cursor is drawn by AMP; needs mousevirt + host bridge):

```powershell
$py = "D:\Espressif\tools\python_env\idf5.5_py3.12_env\Scripts\python.exe"
& $py tools\pcmplay.py --reconnect
& $py tools\mousevirt.py --reconnect
```

Guest: `drv install h:\mousevirt.sys` (once). Without it, a centered cursor still
shows but does not track the host mouse.

## Args

`pcmvirt` / `pcmnull` / `audio`, optional `file.mp3` or directory to scan.

## Open a file

Put `.mp3` under the HostFS share (`build\sd_card\`):

- `amp\song.mp3`
- `music\song.mp3`
- or directly in the share root

Then in AMP: **Eject** button, or keys **A** / **O** → picker → **Enter** (or
double-click a playlist row). **Space** / Play uses the selected playlist entry;
if the list is empty, Play opens the picker.

## Keyboard

| Key | Action |
|-----|--------|
| Tab / Shift+Tab | Focus Main → EQ → Playlist (QVGA: visible panel) |
| Space | Play / pause (opens picker if no tracks) |
| A / O / Eject | Rescan `H:\amp`, `H:\music`, `H:\` and open picker |
| Enter | Play selection (or confirm picker) |
| V | Stop |
| B / N | Next |
| Z / P | Previous (P = preamp select when EQ focused) |
| Left / Right | Seek ±5s |
| Up / Down | Volume (playlist/EQ/picker: list) |
| , / . | Balance |
| R / S | Repeat / shuffle |
| L / W | Load / save `H:\amp\playlist.m3u` |
| E / Q | EQ on/off / reset |
| 0–9 / \` | Select EQ band / preamp |
| Esc | Close picker / quit |

## Skins

Default chrome is painted in RAM (fast). Optional HostFS override (slow on
QEMU HostFS — ~400 KB for VGA) only if a marker file exists:

- `H:\amp\skin\vga\load` plus `main.rgb565` / `eq.rgb565` / `pl.rgb565`
- `H:\amp\skin\qvga\load` plus the same names

Profile is chosen from framebuffer width (`< 400` → QVGA).

Startup shows the UI before opening any MP3; press **Space** / Play to load
the selected playlist entry (`H:\amp\*.mp3`).
